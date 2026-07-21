// Two simulated remote GPU hosts. The coordinator runs only Master and sends
// depth-4 Seeds over stream sockets. Each host runs its own receiver, worker
// threads, Dispatcher, fake GPU, and NodePool.
#include "../Master.hpp"
#include "../RemoteTransport.hpp"
#include "../Worker.hpp"
#include "test_util.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <memory>
#include <sys/socket.h>
#include <thread>
#include <vector>

constexpr size_t BRANCHING = 3;
static std::atomic<uint64_t> g_jobs{0};

std::vector<Move> candidateMoves(const JobState&) {
    std::vector<Move> moves;
    for (size_t i = 0; i < BRANCHING; ++i)
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
}

bool allBugsEaten(const JobState&) { return false; }

void launchCudaBatch(JobState* states, size_t count) {
    for (size_t i = 0; i < count; ++i) states[i].offBoard = 0;
    g_jobs.fetch_add(count, std::memory_order_relaxed);
}

static std::array<int, 2> socketPair() {
    std::array<int, 2> fds{};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) != 0) std::abort();
    return fds;
}

int main() {
    constexpr size_t REMOTE_WORKERS = 2;
    constexpr size_t POOL_CAPACITY = 8000;
    const auto checkpointDirectory = std::filesystem::temp_directory_path() /
        ("labysolver-remote-checkpoint-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(checkpointDirectory);
    durable::CoordinatorCheckpoint checkpoint(checkpointDirectory / "coordinator.chk");

    auto pairA = socketPair();
    auto pairB = socketPair();
    auto coordinatorA = std::make_shared<remote::FramedSocket>(pairA[0]);
    auto hostA = std::make_shared<remote::FramedSocket>(pairA[1]);
    auto coordinatorB = std::make_shared<remote::FramedSocket>(pairB[0]);
    auto hostB = std::make_shared<remote::FramedSocket>(pairB[1]);

    SeedQueue<32> coordinatorSeeds, seedsA, seedsB;
    remote::SeedDistributor distributor(std::vector<remote::SeedDistributor::Peer>{
        {coordinatorA, 2}, {coordinatorB, 1}});
    remote::SeedReceiver receiverA(hostA), receiverB(hostB);
    remote::RemoteMetricsSink metricsA(hostA), metricsB(hostB);
    remote::MetricsReceiver metricsReceiverA(coordinatorA, &checkpoint),
        metricsReceiverB(coordinatorB, &checkpoint);
    SearchInstrumentation instrumentation;
    uint64_t sent = 0, receivedA = 0, receivedB = 0;
    uint64_t samplesA = 0, samplesB = 0;
    std::thread distributorThread([&] { sent = distributor.run(coordinatorSeeds); });
    std::thread receiverThreadA([&] { receivedA = receiverA.run(seedsA); });
    std::thread receiverThreadB([&] { receivedB = receiverB.run(seedsB); });
    std::thread metricsThreadA([&] { samplesA = metricsReceiverA.run(instrumentation); });
    std::thread metricsThreadB([&] { samplesB = metricsReceiverB.run(instrumentation); });

    NodePool coordinatorPool(POOL_CAPACITY), poolA(POOL_CAPACITY), poolB(POOL_CAPACITY);
    Dispatcher coordinatorDispatcher(1), dispatcherA(REMOTE_WORKERS + 1),
        dispatcherB(REMOTE_WORKERS + 1);
    SearchGlobals coordinatorGlobals, globalsA, globalsB;
    coordinatorDispatcher.start();
    dispatcherA.start();
    dispatcherB.start();

    {
        std::vector<Worker> workersA, workersB;
        workersA.reserve(REMOTE_WORKERS);
        workersB.reserve(REMOTE_WORKERS);
        for (size_t i = 0; i < REMOTE_WORKERS; ++i) {
            workersA.emplace_back(i + 1, dispatcherA, poolA, seedsA, globalsA, &metricsA);
            workersB.emplace_back(i + 1, dispatcherB, poolB, seedsB, globalsB, &metricsB);
        }
        std::vector<std::thread> threads;
        for (auto& worker : workersA) threads.emplace_back([&worker] { worker.run(); });
        for (auto& worker : workersB) threads.emplace_back([&worker] { worker.run(); });

        JobState initial{};
        Master master(coordinatorDispatcher, coordinatorPool, coordinatorSeeds,
                      coordinatorGlobals, initial, &instrumentation, {}, &checkpoint);
        master.run();

        distributorThread.join();
        receiverThreadA.join();
        receiverThreadB.join();
        for (auto& thread : threads) thread.join();
        metricsA.finished();
        metricsB.finished();
        metricsThreadA.join();
        metricsThreadB.join();
    } // release worker-local pool caches before leak checks

    coordinatorDispatcher.stop();
    dispatcherA.stop();
    dispatcherB.stop();

    CHECK(sent == 81); // 3^MASTER_DEPTH
    CHECK(receivedA + receivedB == sent);
    CHECK(receivedA == 54); // weighted 2 worker slots : 1 worker slot
    CHECK(receivedB == 27);
    CHECK(samplesA == receivedA);
    CHECK(samplesB == receivedB);
    CHECK(g_jobs.load(std::memory_order_relaxed) == 3279); // sum 3^1..3^7
    CHECK(coordinatorPool.inUseCount() == 0);
    CHECK(poolA.inUseCount() == 0);
    CHECK(poolB.inUseCount() == 0);

    SearchEstimate estimate = instrumentation.snapshot();
    CHECK(estimate.masterFinished);
    CHECK(estimate.completedSeeds == 81);
    CHECK(static_cast<uint64_t>(estimate.estimatedTotalJobs) == 3279);
    auto recovered = checkpoint.snapshot();
    CHECK(recovered.generatedSeeds == 81);
    CHECK(recovered.pending.empty());
    CHECK(recovered.masterFinished);

    std::filesystem::remove_all(checkpointDirectory);

    REPORT();
}
