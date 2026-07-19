#include "../Dispatcher.hpp"
#include "../NodePool.hpp"
#include "../SeedQueue.hpp"
#include "../Worker.hpp"
#include "test_util.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

constexpr size_t CANCEL_BRANCHING = 20;
static std::atomic<uint64_t> g_processed{0};

std::vector<Move> candidateMoves(const JobState&) {
    std::vector<Move> moves;
    for (size_t i = 0; i < CANCEL_BRANCHING; ++i)
        moves.push_back({static_cast<uint8_t>(i), 0});
    return moves;
}

void applyMove(JobState& out, const JobState& parent, Move move) {
    out = parent;
    out.insertPoint = move.insertPoint;
    ++out.boardBytes[7];
}

bool allBugsEaten(const JobState&) { return false; }

void launchCudaBatch(JobState* states, size_t count) {
    std::this_thread::sleep_for(std::chrono::microseconds(50));
    for (size_t i = 0; i < count; ++i) states[i].offBoard = 0;
    g_processed.fetch_add(count, std::memory_order_relaxed);
}

int main() {
    constexpr size_t ITERATIONS = 10;
    constexpr size_t WORKERS = 4;
    for (size_t iteration = 0; iteration < ITERATIONS; ++iteration) {
        g_processed.store(0);
        NodePool pool(24000);
        Dispatcher dispatcher(WORKERS + 1);
        SeedQueue<32> seeds;
        SearchGlobals globals;
        WorkerSchedulerConfig config;
        config.maxJobsInFlight = 500;
        config.resumeJobsInFlight = 250;
        config.maxResidentNodes = 2000;
        config.escapeBranching = CANCEL_BRANCHING;

        for (size_t i = 0; i < WORKERS * 2; ++i) {
            Seed seed{};
            seed.depth = 4;
            seed.state.boardBytes[7] = 4;
            seeds.push(seed);
        }
        dispatcher.start();
        std::vector<Worker> workers;
        workers.reserve(WORKERS);
        for (size_t i = 0; i < WORKERS; ++i)
            workers.emplace_back(i + 1, dispatcher, pool, seeds, globals, nullptr, config);
        std::vector<std::thread> threads;
        for (Worker& worker : workers) threads.emplace_back([&worker] { worker.run(); });

        while (g_processed.load(std::memory_order_relaxed) < 100)
            std::this_thread::yield();
        globals.solutionFound.store(true, std::memory_order_relaxed);
        seeds.abort();

        for (std::thread& thread : threads) thread.join();
        dispatcher.abandonProducer(0); // unused master producer
        dispatcher.stop();
        CHECK(g_processed.load() >= 100);
        // Retained count may be non-zero: cancellation must not call untested
        // nodes negative merely to make the arena counter reach zero.
        CHECK(pool.inUseCount() <= static_cast<long>(pool.capacity()));
    }
    REPORT();
}
