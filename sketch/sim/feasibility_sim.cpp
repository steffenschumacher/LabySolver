// Configurable CPU-pipeline feasibility simulation.
//
// Uses the production Master/Worker/Dispatcher/NodePool/SeedQueue classes
// with a deterministic synthetic search tree and a fake CUDA batch call.
// It deliberately runs the tree to exhaustion so every queue, producer and
// cleanup path is exercised; no real board representation or GPU is needed.

#include "../Master.hpp"
#include "../Worker.hpp"
#include "../Dispatcher.hpp"
#include "../NodePool.hpp"
#include "../SeedQueue.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Config {
    size_t workers = 10;
    size_t branching = 150;
    double survivalRate = 0.03;
    uint64_t kernelDelayUs = 100;
    uint64_t seed = 1;
    uint64_t progressMs = 1000;
    uint64_t maxSeconds = 0;
    size_t inflightHigh = 1000;
    size_t inflightLow = 750;
    size_t residentLimit = 5000;
};

struct KernelStats {
    uint64_t jobs = 0;
    uint64_t batches = 0;
    uint64_t fullBatches = 0;
    uint64_t minBatch = std::numeric_limits<uint64_t>::max();
    uint64_t maxBatch = 0;
};

Config g_config;
KernelStats g_kernelStats;
std::atomic<uint64_t> g_progressJobs{0};

uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

uint64_t readHash(const JobState& state) {
    uint64_t value;
    std::memcpy(&value, state.boardBytes, sizeof(value));
    return value;
}

void writeHash(JobState& state, uint64_t value) {
    std::memcpy(state.boardBytes, &value, sizeof(value));
}

uint64_t parseUnsigned(const char* option, const char* text) {
    char* end = nullptr;
    unsigned long long value = std::strtoull(text, &end, 10);
    if (!text[0] || !end || *end != '\0')
        throw std::runtime_error(std::string("invalid value for ") + option + ": " + text);
    return static_cast<uint64_t>(value);
}

double parseRate(const char* text) {
    char* end = nullptr;
    double value = std::strtod(text, &end);
    if (!text[0] || !end || *end != '\0' || !std::isfinite(value) || value < 0.0 || value > 1.0)
        throw std::runtime_error(std::string("invalid --survival value: ") + text);
    return value;
}

void usage(const char* program) {
    std::printf(
        "Usage: %s [options]\n"
        "  --workers N       Worker threads (default: 10)\n"
        "  --branching N     Candidates generated per expansion, 1..1024 (default: 150)\n"
        "  --survival RATE   Deterministic post-kernel survivor fraction, 0..1 (default: 0.03)\n"
        "  --kernel-us N     Fake CUDA latency per dispatcher batch (default: 100)\n"
        "  --seed N          Deterministic workload seed (default: 1)\n"
        "  --progress-ms N   Live reporting interval; 0 disables (default: 1000)\n"
        "  --max-seconds N   Stop after N seconds; 0 runs to exhaustion (default: 0)\n"
        "  --inflight-high N Pause worker submission at this many jobs (default: 1000)\n"
        "  --inflight-low N  Resume worker submission below this many jobs (default: 750)\n"
        "  --resident N      Resident-node budget per worker (default: 5000)\n"
        "  --help            Show this help\n",
        program);
}

Config parseArgs(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help") {
            usage(argv[0]);
            std::exit(0);
        }
        if (i + 1 >= argc) throw std::runtime_error("missing value for " + arg);
        const char* value = argv[++i];
        if (arg == "--workers") config.workers = parseUnsigned(arg.c_str(), value);
        else if (arg == "--branching") config.branching = parseUnsigned(arg.c_str(), value);
        else if (arg == "--survival") config.survivalRate = parseRate(value);
        else if (arg == "--kernel-us") config.kernelDelayUs = parseUnsigned(arg.c_str(), value);
        else if (arg == "--seed") config.seed = parseUnsigned(arg.c_str(), value);
        else if (arg == "--progress-ms") config.progressMs = parseUnsigned(arg.c_str(), value);
        else if (arg == "--max-seconds") config.maxSeconds = parseUnsigned(arg.c_str(), value);
        else if (arg == "--inflight-high") config.inflightHigh = parseUnsigned(arg.c_str(), value);
        else if (arg == "--inflight-low") config.inflightLow = parseUnsigned(arg.c_str(), value);
        else if (arg == "--resident") config.residentLimit = parseUnsigned(arg.c_str(), value);
        else throw std::runtime_error("unknown option: " + arg);
    }
    if (config.workers == 0 || config.workers > 256)
        throw std::runtime_error("--workers must be in the range 1..256");
    if (config.branching == 0 || config.branching > 1024)
        throw std::runtime_error("--branching must be in the range 1..1024");
    if (config.inflightHigh == 0 || config.inflightLow >= config.inflightHigh)
        throw std::runtime_error("in-flight limits require 0 <= low < high");
    return config;
}

} // namespace

std::vector<Move> candidateMoves(const JobState&) {
    std::vector<Move> moves;
    moves.reserve(g_config.branching);
    for (size_t i = 0; i < g_config.branching; ++i) {
        moves.push_back(
            {static_cast<uint8_t>(i & 0xff), static_cast<uint8_t>((i >> 8) & 0x03)});
    }
    return moves;
}

void applyMove(JobState& out, const JobState& parent, Move move) {
    out = parent;
    uint64_t moveId = static_cast<uint64_t>(move.insertPoint) |
                      (static_cast<uint64_t>(move.orientation) << 8);
    uint64_t depth = static_cast<uint64_t>(parent.boardBytes[8] + 1);
    writeHash(out, mix(readHash(parent) ^ mix(moveId + depth * 1024 + g_config.seed)));
    out.boardBytes[8] = static_cast<uint8_t>(depth);
    out.insertPoint = move.insertPoint;
    out.orientation = move.orientation;
    out.reachableMask = 0;
    out.bugsEatenMask = 0;
    out.offBoard = 0;
}

bool allBugsEaten(const JobState&) {
    // Exhaustive mode is intentional: it verifies normal completion and that
    // every retained node returns to the pool.
    return false;
}

void launchCudaBatch(JobState* states, size_t n) {
    if (g_config.kernelDelayUs)
        std::this_thread::sleep_for(std::chrono::microseconds(g_config.kernelDelayUs));

    const long double cutoff = g_config.survivalRate *
                               static_cast<long double>(std::numeric_limits<uint64_t>::max());
    for (size_t i = 0; i < n; ++i) {
        const uint64_t verdict = mix(readHash(states[i]) ^ 0xd1b54a32d192ed03ULL);
        states[i].offBoard = static_cast<long double>(verdict) <= cutoff ? 0 : 1;
    }

    g_kernelStats.jobs += n;
    g_progressJobs.fetch_add(n, std::memory_order_relaxed);
    ++g_kernelStats.batches;
    if (n == MAX_BATCH) ++g_kernelStats.fullBatches;
    g_kernelStats.minBatch = std::min<uint64_t>(g_kernelStats.minBatch, n);
    g_kernelStats.maxBatch = std::max<uint64_t>(g_kernelStats.maxBatch, n);
}

int main(int argc, char** argv) {
    try {
        g_config = parseArgs(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        usage(argv[0]);
        return 2;
    }

    const size_t producers = g_config.workers + 1;
    WorkerSchedulerConfig workerConfig;
    workerConfig.escapeBranching = g_config.branching;
    workerConfig.maxJobsInFlight = g_config.inflightHigh;
    workerConfig.resumeJobsInFlight = g_config.inflightLow;
    workerConfig.maxResidentNodes = g_config.residentLimit;
    const size_t masterAllowance = static_cast<size_t>(MAX_DEPTH) *
                                   (g_config.branching + FLUSH_THRESHOLD);
    const size_t poolCapacity = g_config.workers *
                                    (workerConfig.maxResidentNodes + FLUSH_THRESHOLD) +
                                masterAllowance + 8 * MAX_BATCH;

    NodePool pool(poolCapacity);
    Dispatcher dispatcher(producers);
    SeedQueue<32> seedQueue;
    SearchGlobals globals;
    SearchInstrumentation instrumentation;
    JobState initial{};
    writeHash(initial, mix(g_config.seed));

    std::atomic<bool> searchThreadsDone{false};
    std::atomic<bool> timeLimitReached{false};
    std::atomic<size_t> workersDone{0};
    std::atomic<bool> masterDone{false};
    std::mutex reporterMutex;
    std::condition_variable reporterWake;
    dispatcher.start();

    const std::clock_t cpuStart = std::clock();
    const auto wallStart = std::chrono::steady_clock::now();

    std::thread reporterThread;
    if (g_config.progressMs || g_config.maxSeconds) {
        reporterThread = std::thread([&] {
            uint64_t previousJobs = 0;
            auto previousTime = wallStart;
            const uint64_t wakeMs = g_config.progressMs ? g_config.progressMs : 100;
            std::unique_lock<std::mutex> lock(reporterMutex);
            while (!searchThreadsDone.load(std::memory_order_relaxed)) {
                reporterWake.wait_for(lock, std::chrono::milliseconds(wakeMs));
                if (searchThreadsDone.load(std::memory_order_relaxed)) break;
                const auto now = std::chrono::steady_clock::now();
                const double elapsed = std::chrono::duration<double>(now - wallStart).count();

                if (g_config.progressMs) {
                    const uint64_t jobs = g_progressJobs.load(std::memory_order_relaxed);
                    const double interval = std::chrono::duration<double>(now - previousTime).count();
                    const double rate = (jobs - previousJobs) / std::max(interval, 1e-9);
                    SearchEstimate estimate = instrumentation.snapshot();
                    std::printf(
                        "[%.1fs] jobs=%llu rate=%.0f/s samples=%llu seeds=%.0Lf/%.0Lf "
                        "estimated-total=%.0Lf master=%s workers-done=%zu/%zu\n",
                        elapsed, static_cast<unsigned long long>(jobs), rate,
                        static_cast<unsigned long long>(estimate.completedSeeds),
                        static_cast<long double>(estimate.completedSeeds), estimate.estimatedSeeds,
                        estimate.estimatedTotalJobs, masterDone.load() ? "stopped" : "running",
                        workersDone.load(), g_config.workers);
                    std::fflush(stdout);
                    previousJobs = jobs;
                    previousTime = now;
                }

                if (g_config.maxSeconds && elapsed >= static_cast<double>(g_config.maxSeconds) &&
                    !timeLimitReached.exchange(true, std::memory_order_relaxed)) {
                    globals.solutionFound.store(true, std::memory_order_relaxed);
                    seedQueue.abort();
                }
            }
        });
    }

    std::thread masterThread([&] {
        Master master(dispatcher, pool, seedQueue, globals, initial, &instrumentation);
        master.run();
        masterDone.store(true, std::memory_order_relaxed);
    });

    std::vector<Worker> workers;
    workers.reserve(g_config.workers);
    for (size_t i = 0; i < g_config.workers; ++i)
        workers.emplace_back(i + 1, dispatcher, pool, seedQueue, globals, &instrumentation,
                             workerConfig);

    std::vector<std::thread> workerThreads;
    workerThreads.reserve(g_config.workers);
    for (auto& worker : workers) workerThreads.emplace_back([&worker, &workersDone] {
        worker.run();
        workersDone.fetch_add(1, std::memory_order_relaxed);
    });

    masterThread.join();
    for (auto& thread : workerThreads) thread.join();
    searchThreadsDone.store(true, std::memory_order_relaxed);
    reporterWake.notify_all();
    if (reporterThread.joinable()) reporterThread.join();
    dispatcher.stop();
    WorkerSchedulerStats combinedStats;
    for (const Worker& worker : workers) {
        const WorkerSchedulerStats& workerStats = worker.schedulerStats();
        combinedStats.expansionsSubmitted += workerStats.expansionsSubmitted;
        combinedStats.jobsSubmitted += workerStats.jobsSubmitted;
        combinedStats.resultsReceived += workerStats.resultsReceived;
        combinedStats.completedSeeds += workerStats.completedSeeds;
        combinedStats.throttleTransitions += workerStats.throttleTransitions;
        combinedStats.peakJobsInFlight =
            std::max(combinedStats.peakJobsInFlight, workerStats.peakJobsInFlight);
        combinedStats.peakReadyNodes =
            std::max(combinedStats.peakReadyNodes, workerStats.peakReadyNodes);
        combinedStats.peakResidentNodes =
            std::max(combinedStats.peakResidentNodes, workerStats.peakResidentNodes);
    }
    // Destroy ThreadLocalPool instances before measuring final shared-pool
    // usage; their unused refill caches are intentionally retained for the
    // lifetime of each Worker object.
    workers.clear();

    const auto wallEnd = std::chrono::steady_clock::now();
    const std::clock_t cpuEnd = std::clock();
    const double wallSeconds = std::chrono::duration<double>(wallEnd - wallStart).count();
    const double cpuSeconds = static_cast<double>(cpuEnd - cpuStart) / CLOCKS_PER_SEC;
    const double averageBatch = g_kernelStats.batches
                                    ? static_cast<double>(g_kernelStats.jobs) / g_kernelStats.batches
                                    : 0.0;
    const double jobsPerSecond = g_kernelStats.jobs / std::max(wallSeconds, 1e-9);
    const double averageCores = cpuSeconds / std::max(wallSeconds, 1e-9);
    const double fullBatchPercent = g_kernelStats.batches
                                        ? 100.0 * g_kernelStats.fullBatches / g_kernelStats.batches
                                        : 0.0;

    std::printf("CPU pipeline feasibility simulation\n");
    std::printf("  configuration: workers=%zu branching=%zu survival=%.4f kernel-delay=%llu us seed=%llu "
                "inflight=%zu/%zu resident=%zu\n",
                g_config.workers, g_config.branching, g_config.survivalRate,
                static_cast<unsigned long long>(g_config.kernelDelayUs),
                static_cast<unsigned long long>(g_config.seed), g_config.inflightHigh,
                g_config.inflightLow, g_config.residentLimit);
    std::printf("  elapsed: %.3f s wall, %.3f s process CPU (%.2f average cores)\n",
                wallSeconds, cpuSeconds, averageCores);
    std::printf("  throughput: %llu jobs, %.0f jobs/s\n",
                static_cast<unsigned long long>(g_kernelStats.jobs), jobsPerSecond);
    std::printf("  batching: %llu launches, avg %.1f, min %llu, max %llu, %.1f%% full\n",
                static_cast<unsigned long long>(g_kernelStats.batches), averageBatch,
                static_cast<unsigned long long>(g_kernelStats.batches ? g_kernelStats.minBatch : 0),
                static_cast<unsigned long long>(g_kernelStats.maxBatch), fullBatchPercent);
    DispatcherStatsSnapshot pipelineStats = dispatcher.stats();
    std::printf("  pipeline: %llu prepared, %llu full, %llu deadline flushes\n",
                static_cast<unsigned long long>(pipelineStats.batchesPrepared),
                static_cast<unsigned long long>(pipelineStats.fullBatches),
                static_cast<unsigned long long>(pipelineStats.deadlineFlushes));
    std::printf("  node pool: peak %ld / %zu (%.1f%%), final in-use %ld\n", pool.peakInUse(),
                pool.capacity(), 100.0 * pool.peakInUse() / pool.capacity(), pool.inUseCount());
    SearchEstimate estimate = instrumentation.snapshot();
    std::printf("  search size: estimated %.0Lf total jobs, %.0Lf viable seeds, from %llu completed samples\n",
                estimate.estimatedTotalJobs,
                estimate.estimatedSeeds,
                static_cast<unsigned long long>(estimate.completedSeeds));
    for (size_t depth = 1; depth < INSTRUMENTED_DEPTHS; ++depth) {
        std::printf("    depth %zu: observed %llu%s, estimated %.0Lf, avg branching %.2Lf\n", depth,
                    static_cast<unsigned long long>(estimate.observedJobs[depth]),
                    estimate.observedJobsOverflowed[depth] ? "+ (saturated)" : "",
                    estimate.estimatedJobs[depth], estimate.averageBranching[depth]);
    }
    std::printf("  worker scheduler: completed-seeds=%llu peak/worker in-flight=%zu ready=%zu "
                "resident=%zu throttle-transitions=%llu\n",
                static_cast<unsigned long long>(combinedStats.completedSeeds),
                combinedStats.peakJobsInFlight, combinedStats.peakReadyNodes,
                combinedStats.peakResidentNodes,
                static_cast<unsigned long long>(combinedStats.throttleTransitions));

    if (timeLimitReached.load(std::memory_order_relaxed)) {
        std::printf("  result: stopped at the configured time limit; retained DFS nodes are released at process exit\n");
        return 0;
    }
    if (globals.solutionFound.load() || pool.inUseCount() != 0) {
        std::fprintf(stderr, "simulation invariant failed: expected exhaustive no-solution cleanup\n");
        return 1;
    }
    return 0;
}
