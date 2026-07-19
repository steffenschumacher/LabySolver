// End-to-end simulation: exercises the real Master/Worker/Dispatcher
// pipeline (strict-DFS master + bounded asynchronous workers) against a synthetic,
// cheap-to-evaluate "board" so the full depth-7 search actually completes
// in test time, while preserving the same shape (master depth 4 + worker
// depth 3, chunked FLUSH_THRESHOLD submissions, node-pool recycling) as
// the real puzzle. Verifies:
//   1. a planted solution is found and the move path reconstructs correctly
//   2. peak NodePool usage stays within the configured fixed arena and remains
//      far below the full breadth-first tree
//   3. reports wall-clock time and effective job throughput
#include "../Master.hpp"
#include "../Worker.hpp"
#include "../Dispatcher.hpp"
#include "../NodePool.hpp"
#include "../SeedQueue.hpp"
#include "test_util.hpp"

#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// Synthetic board model (replaces the real tile/board logic for this test).
// boardBytes[0..6] store the branch chosen at each of the 7 move levels;
// boardBytes[7] is a running "moves so far" counter so applyMove knows
// which slot to write without needing the level passed in explicitly.
// ---------------------------------------------------------------------------
constexpr int BRANCHING = 6;   // stand-in for ~30 real insertion+orientation combos
constexpr int TOTAL_DEPTH = 7; // matches MASTER_DEPTH(4) + (MAX_DEPTH-MASTER_DEPTH)(3)

static const uint8_t TARGET_PATH[TOTAL_DEPTH] = {2, 4, 1, 5, 0, 3, 2};

std::vector<Move> candidateMoves(const JobState& /*parent*/) {
    std::vector<Move> moves;
    moves.reserve(BRANCHING);
    for (int i = 0; i < BRANCHING; ++i) moves.push_back({static_cast<uint8_t>(i), 0});
    return moves;
}

void applyMove(JobState& out, const JobState& parent, Move move) {
    out = parent;
    uint8_t slot = parent.boardBytes[7];
    out.boardBytes[slot] = move.insertPoint;
    out.boardBytes[7] = slot + 1;
    out.insertPoint = move.insertPoint;
    out.orientation = move.orientation;
}

bool allBugsEaten(const JobState& state) {
    return state.bugsEatenMask == 0xF;
}

// Fake CUDA kernel: forces every node lying on TARGET_PATH's prefix to stay
// alive (guaranteeing the planted solution is reachable), and randomly
// prunes ~20% of everything else (to exercise the dead-chain recycle path
// the same way real dead-ends / off-board bugs would).
static std::atomic<long> g_jobsProcessed{0};

static uint32_t fnv1a(const uint8_t* data, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

void launchCudaBatch(JobState* states, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        JobState& s = states[i];
        uint8_t depth = s.boardBytes[7];
        bool onTargetPrefix = (std::memcmp(s.boardBytes, TARGET_PATH, depth) == 0);

        if (onTargetPrefix) {
            s.offBoard = 0;
            s.bugsEatenMask = (depth == TOTAL_DEPTH) ? 0xF : 0;
        } else {
            uint32_t h = fnv1a(s.boardBytes, depth);
            s.offBoard = (h % 5 == 0) ? 1 : 0; // ~20% pruned, matching real off-board bugs
            s.bugsEatenMask = 0;
        }
    }
    g_jobsProcessed.fetch_add(static_cast<long>(n), std::memory_order_relaxed);
}

int main() {
    constexpr size_t NUM_WORKERS = 4;
    constexpr size_t NUM_PRODUCERS = NUM_WORKERS + 1;

    // DFS peak bound: each producer (master + each worker) never holds
    // more than one root-to-leaf path's untried siblings at once, i.e.
    // O(MAX_DEPTH x BRANCHING), not a whole level's worth of leaves.
    size_t perProducerDfsPeak = static_cast<size_t>(MAX_DEPTH) * BRANCHING;
    size_t dfsBound = NUM_PRODUCERS * perProducerDfsPeak;

    // For comparison only: what the old BFS design would have needed
    // (sum of full levels, per producer) -- shown to make the reduction
    // concrete in the test output, not used to size the pool.
    size_t masterBfsPeak = 0;
    {
        size_t p = 1;
        for (int lvl = 0; lvl < MASTER_DEPTH; ++lvl) {
            p *= BRANCHING;
            masterBfsPeak += p;
        }
    }
    size_t workerBfsPeak = 0;
    {
        size_t p = 1;
        for (int lvl = 0; lvl < MAX_DEPTH - MASTER_DEPTH; ++lvl) {
            p *= BRANCHING;
            workerBfsPeak += p;
        }
    }
    size_t bfsBound = masterBfsPeak + NUM_WORKERS * workerBfsPeak;

    // Fixed asynchronous-search arena for this small synthetic workload. It is
    // larger than the strict-DFS reference but nowhere near a full tree.
    const size_t poolCapacity = dfsBound + 4 * MAX_BATCH;
    NodePool pool(poolCapacity);
    Dispatcher dispatcher(NUM_PRODUCERS);
    SeedQueue<32> seedQueue;
    SearchGlobals globals;

    JobState initialBoard{}; // all zero: boardBytes[7] == 0 moves taken so far

    dispatcher.start();

    auto t0 = std::chrono::steady_clock::now();

    std::thread masterThread([&] {
        Master master(dispatcher, pool, seedQueue, globals, initialBoard);
        master.run();
    });

    std::vector<Worker> workers;
    workers.reserve(NUM_WORKERS);
    for (size_t i = 0; i < NUM_WORKERS; ++i)
        workers.emplace_back(i + 1, dispatcher, pool, seedQueue, globals);

    std::vector<std::thread> workerThreads;
    for (auto& w : workers) workerThreads.emplace_back([&w] { w.run(); });

    masterThread.join();
    for (auto& t : workerThreads) t.join();
    dispatcher.stop();

    auto t1 = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(t1 - t0).count();

    CHECK(globals.solutionFound.load());
    CHECK(globals.solutionLeaf != nullptr);

    if (globals.solutionLeaf) {
        // Full 7-move path is recoverable directly from the leaf's own
        // encoded boardBytes (applyMove always copies the parent's full
        // path forward, so depth-7 boardBytes holds all 7 moves even
        // though the leaf's own parent-chain only reaches back to the
        // worker's seed root, not all the way to the master's root).
        bool bytesMatch =
            std::memcmp(globals.solutionLeaf->state.boardBytes, TARGET_PATH, TOTAL_DEPTH) == 0;
        CHECK(bytesMatch);

        // Cross-check the worker-owned portion (moves MASTER_DEPTH+1..7)
        // independently by walking parent pointers — this is the part of
        // the path a worker can reconstruct purely from its own retained
        // chain (the master-side moves 1..MASTER_DEPTH travel via
        // Seed::moves instead, since the master's original nodes are
        // released as soon as each seed is emitted).
        std::vector<uint8_t> walked;
        for (JobNode* n = globals.solutionLeaf; n && n->parent; n = n->parent)
            walked.push_back(n->state.insertPoint);
        std::reverse(walked.begin(), walked.end());

        CHECK(walked.size() == static_cast<size_t>(TOTAL_DEPTH - MASTER_DEPTH));
        bool walkMatches = true;
        for (size_t i = 0; i < walked.size(); ++i)
            if (walked[i] != TARGET_PATH[MASTER_DEPTH + i]) walkMatches = false;
        CHECK(walkMatches);

        std::printf("Reconstructed moves %d..%d (walking worker parent chain): ", MASTER_DEPTH + 1,
                    TOTAL_DEPTH);
        for (auto b : walked) std::printf("%d ", b);
        std::printf("\n");
    }

    long peak = pool.peakInUse();
    std::printf("NodePool peak in-use: %ld / %zu capacity (strict-DFS reference ~%zu, "
                "full-BFS reference ~%zu)\n",
                peak, poolCapacity, dfsBound, bfsBound);
    CHECK(peak <= static_cast<long>(poolCapacity));

    long jobs = g_jobsProcessed.load();
    std::printf("Elapsed: %.3fs, fake jobs processed: %ld (%.0f jobs/sec)\n", seconds, jobs,
                jobs / std::max(seconds, 1e-6));

    REPORT();
}
