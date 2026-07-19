#include "../Dispatcher.hpp"
#include "../NodePool.hpp"
#include "../SeedQueue.hpp"
#include "../Worker.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

static size_t g_branching = 4;
static std::mutex g_mutex;
static std::vector<uint8_t> g_submissionDepths;
static std::unordered_set<uint64_t> g_seenPaths;
static std::atomic<uint64_t> g_jobs{0};
static std::atomic<uint64_t> g_duplicates{0};

std::vector<Move> candidateMoves(const JobState&) {
    std::vector<Move> moves;
    for (size_t i = 0; i < g_branching; ++i)
        moves.push_back({static_cast<uint8_t>(i), 0});
    return moves;
}

void applyMove(JobState& out, const JobState& parent, Move move) {
    out = parent;
    uint8_t depth = parent.boardBytes[7];
    out.boardBytes[depth] = move.insertPoint;
    out.boardBytes[7] = depth + 1;
    out.insertPoint = move.insertPoint;
    out.orientation = 0;
    out.offBoard = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_submissionDepths.push_back(depth + 1);
}

bool allBugsEaten(const JobState&) { return false; }

void launchCudaBatch(JobState* states, size_t count) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < count; ++i) {
        JobState& state = states[i];
        state.offBoard = 0;
        uint64_t path = state.boardBytes[7];
        for (size_t depth = 4; depth < state.boardBytes[7]; ++depth)
            path = path * 32 + state.boardBytes[depth] + 1;
        if (!g_seenPaths.insert(path).second) g_duplicates.fetch_add(1);
    }
    g_jobs.fetch_add(count);
}

int main() {
    constexpr uint64_t EXPECTED_JOBS = 4 + 16 + 64;
    NodePool pool(500);
    Dispatcher dispatcher(2); // producer 0 unused, worker is producer 1
    SeedQueue<32> seeds;
    SearchGlobals globals;
    SearchInstrumentation instrumentation;
    WorkerSchedulerConfig config;
    config.maxJobsInFlight = 8;
    config.resumeJobsInFlight = 4;
    config.maxResidentNodes = 80;
    config.escapeBranching = g_branching;

    Seed seed{};
    seed.depth = 4;
    seed.state.boardBytes[7] = 4;
    seeds.push(seed);
    seeds.finished();
    dispatcher.start();

    WorkerSchedulerStats stats;
    {
        Worker worker(1, dispatcher, pool, seeds, globals, &instrumentation, config);
        std::thread thread([&] { worker.run(); });
        thread.join();
        stats = worker.schedulerStats();
    }
    dispatcher.stop();

    CHECK(!globals.solutionFound.load());
    CHECK(g_jobs.load() == EXPECTED_JOBS);
    CHECK(g_seenPaths.size() == EXPECTED_JOBS);
    CHECK(g_duplicates.load() == 0);
    CHECK(stats.jobsSubmitted == EXPECTED_JOBS);
    CHECK(stats.resultsReceived == EXPECTED_JOBS);
    CHECK(stats.completedSeeds == 1);
    CHECK(stats.peakJobsInFlight <= config.maxJobsInFlight);
    CHECK(stats.peakResidentNodes <= config.maxResidentNodes);
    CHECK(stats.throttleTransitions > 0);
    CHECK(pool.inUseCount() == 0);

    // With high-water 8, only two of the four depth-5 parents can submit their
    // four depth-6 children before collection. Once depth-6 results arrive,
    // deepest-first scheduling must submit depth-7 work before expanding the
    // remaining depth-5 parents.
    auto firstDepth7 = std::find(g_submissionDepths.begin(), g_submissionDepths.end(), 7);
    CHECK(firstDepth7 != g_submissionDepths.end());
    size_t depth6BeforeDepth7 =
        static_cast<size_t>(std::count(g_submissionDepths.begin(), firstDepth7, 6));
    CHECK(depth6BeforeDepth7 <= config.maxJobsInFlight);
    CHECK(depth6BeforeDepth7 < 16);

    SearchEstimate estimate = instrumentation.snapshot();
    CHECK(estimate.completedSeeds == 1);
    CHECK(estimate.observedJobs[5] == 4);
    CHECK(estimate.observedJobs[6] == 16);
    CHECK(estimate.observedJobs[7] == 64);

    std::printf("async worker: jobs=%llu peak-inflight=%zu peak-ready=%zu peak-resident=%zu "
                "throttle-transitions=%llu\n",
                static_cast<unsigned long long>(stats.jobsSubmitted), stats.peakJobsInFlight,
                stats.peakReadyNodes, stats.peakResidentNodes,
                static_cast<unsigned long long>(stats.throttleTransitions));

    // Higher-branching memory stress: 20 + 20^2 + 20^3 = 8420 jobs are
    // exhaustively tested while a deliberately tight 220-node resident budget
    // forces repeated pause/complete/cascade cycles.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_submissionDepths.clear();
        g_seenPaths.clear();
    }
    g_jobs.store(0);
    g_duplicates.store(0);
    g_branching = 20;
    NodePool stressPool(1000);
    Dispatcher stressDispatcher(2);
    SeedQueue<32> stressSeeds;
    SearchGlobals stressGlobals;
    WorkerSchedulerConfig stressConfig;
    stressConfig.maxJobsInFlight = 100;
    stressConfig.resumeJobsInFlight = 60;
    stressConfig.maxResidentNodes = 220;
    stressConfig.escapeBranching = g_branching;
    Seed stressSeed{};
    stressSeed.depth = 4;
    stressSeed.state.boardBytes[7] = 4;
    stressSeeds.push(stressSeed);
    stressSeeds.finished();
    stressDispatcher.start();
    WorkerSchedulerStats stressStats;
    {
        Worker worker(1, stressDispatcher, stressPool, stressSeeds, stressGlobals, nullptr,
                      stressConfig);
        std::thread thread([&] { worker.run(); });
        thread.join();
        stressStats = worker.schedulerStats();
    }
    stressDispatcher.stop();
    CHECK(g_jobs.load() == 8420);
    CHECK(g_seenPaths.size() == 8420);
    CHECK(g_duplicates.load() == 0);
    CHECK(stressStats.peakJobsInFlight <= stressConfig.maxJobsInFlight);
    CHECK(stressStats.peakResidentNodes <= stressConfig.maxResidentNodes);
    CHECK(stressStats.completedSeeds == 1);
    CHECK(stressPool.inUseCount() == 0);

    // Concurrent solution publication retains exactly one leaf and publishes
    // it before the atomic flag becomes visible.
    SearchGlobals publication;
    std::vector<JobNode> contenders(16);
    std::atomic<size_t> publishers{0};
    std::vector<std::thread> publicationThreads;
    for (JobNode& contender : contenders) {
        publicationThreads.emplace_back([&publication, &publishers, &contender] {
            if (publication.publishSolution(&contender)) publishers.fetch_add(1);
        });
    }
    for (std::thread& thread : publicationThreads) thread.join();
    CHECK(publication.solutionFound.load(std::memory_order_acquire));
    CHECK(publication.solutionLeaf != nullptr);
    CHECK(publishers.load() == 1);

    REPORT();
}
