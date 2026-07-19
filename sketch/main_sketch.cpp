// Wiring sketch: not a complete/compilable program (candidateMoves,
// applyMove, allBugsEaten and launchCudaBatch are declared but not
// defined — plug in your real board logic and CUDA call). Shows how the
// pieces in this directory connect: NodePool -> Dispatcher -> Master/
// Workers -> SeedQueue, plus the three-stage dispatcher pipeline.

#include "JobState.hpp"
#include "NodePool.hpp"
#include "Dispatcher.hpp"
#include "SeedQueue.hpp"
#include "Master.hpp"
#include "Worker.hpp"
#include <thread>
#include <vector>
#include <atomic>

constexpr size_t NUM_WORKERS = 10;

// Size generously: master's own peak (~28k concurrent) + each worker's
// peak (~sum of levels 4..7, ~840k) x NUM_WORKERS, plus slack.
constexpr size_t NODE_POOL_CAPACITY = (NUM_WORKERS + 1) * 900'000;

int main() {
    JobState initialBoard{}; // fill in from the level's starting layout

    NodePool pool(NODE_POOL_CAPACITY);
    Dispatcher dispatcher(NUM_WORKERS + 1); // producer 0 = master, 1..N = workers
    SeedQueue<32> seedQueue;
    SearchGlobals globals;

    dispatcher.start();

    std::thread masterThread([&] {
        Master master(dispatcher, pool, seedQueue, globals, initialBoard);
        master.run();
    });

    std::vector<std::thread> workerThreads;
    std::vector<Worker> workers;
    workers.reserve(NUM_WORKERS);
    for (size_t i = 0; i < NUM_WORKERS; ++i) {
        // Worker producer ids are 1..NUM_WORKERS (0 is master).
        workers.emplace_back(i + 1, dispatcher, pool, seedQueue, globals);
    }
    for (auto& w : workers) workerThreads.emplace_back([&w] { w.run(); });

    masterThread.join();
    for (auto& t : workerThreads) t.join();
    dispatcher.stop();

    if (globals.solutionFound.load()) {
        // Walk globals.solutionLeaf->parent chain (+ the seed's stored
        // moves[3]) to print/reconstruct the full 7-move solution.
    } else {
        // Exhausted the whole tree — no solution within the move limit.
    }
    return 0;
}
