#pragma once
#include "Chain.hpp"
#include "NodePool.hpp"
#include "Dispatcher.hpp"
#include "SeedQueue.hpp"
#include <atomic>
#include <vector>

constexpr size_t FLUSH_THRESHOLD = 200; // producer -> dispatcher chunk size
constexpr int MAX_DEPTH = 7;

// Shared across master + all workers.
struct SearchGlobals {
    std::atomic<bool> solutionFound{false};
    JobNode* solutionLeaf = nullptr; // valid only after solutionFound == true
};

// ---------------------------------------------------------------------------
// Board-specific hooks — fill these in against your real tile/board model.
// Kept out of the pipeline scaffolding on purpose.
// ---------------------------------------------------------------------------
struct Move {
    uint8_t insertPoint;
    uint8_t orientation;
};

// Up to ~30 legal (insertPoint, orientation) combos for the current loose
// tile, given `parent`'s board state -- or several times that once the
// ladybug's own pre-insertion reposition choice is folded in as part of
// the same move. candidateMoves is exactly where that fold-in happens: it
// should enumerate (ladybug pre-position x insertPoint x orientation), not
// just the tile placements alone.
std::vector<Move> candidateMoves(const JobState& parent);

// Fills `out` with the board resulting from applying `move` to `parent`
// (before CUDA has evaluated reachability — those fields get overwritten
// by the kernel once this job is submitted and processed).
void applyMove(JobState& out, const JobState& parent, Move move);

// True once bugsEatenMask (after CUDA has processed this state) indicates
// all 4 bugs are gone.
bool allBugsEaten(const JobState& state);

// ---------------------------------------------------------------------------
// Worker: explicit-stack depth-first search from each seed, instead of a
// level-by-level breadth-first search.
//
// Why: once the ladybug's own pre-insertion reposition choice is folded
// into the branching factor (~30 tile placements x ~5 average reachable
// lady positions =~ 150), a breadth-first frontier's memory is
// O(branching^depth) -- e.g. 150^3 x 100B =~ 340MB *per worker* just for
// the last 3 moves, and grows by another 150x for every extra level a
// worker owns. An explicit-stack DFS instead only ever holds, per depth,
// the untried siblings of *one* ancestor path -- memory is
// O(depth x branching) =~ 7 x 150 x 100B =~ 105KB per worker, regardless
// of how large branching gets. Total work done (and hence total CUDA
// jobs submitted) is unchanged between BFS and DFS when the search must
// run to exhaustion -- the win here is peak memory, not throughput.
//
// Dead subtrees are released back to the pool the instant they're proven
// dead (per exhausted frame, not in one bulk pass at the end), so memory
// stays flat for the whole run rather than merely bounded at the peak.
// ---------------------------------------------------------------------------
class Worker {
public:
    Worker(size_t id, Dispatcher& dispatcher, NodePool& pool, SeedQueue<32>& seeds,
           SearchGlobals& globals)
        : id(id), dispatcher(dispatcher), localPool(pool, FLUSH_THRESHOLD), seeds(seeds),
          globals(globals) {}

    void run() {
        Seed seed;
        while (!globals.solutionFound.load(std::memory_order_relaxed) && seeds.pop(seed)) {
            exploreFromSeed(seed);
        }
    }

private:
    // One entry per depth below the seed root: the untried siblings at
    // that depth, plus the parent node whose children they are (so it can
    // be released the instant this frame is fully exhausted, without
    // waiting for anything else).
    struct Frame {
        JobNode* parent;
        Chain remaining;
    };

    void exploreFromSeed(const Seed& seed) {
        JobNode* root = localPool.alloc();
        root->state = seed.state;
        root->parent = nullptr;
        root->level = seed.depth; // master tells us how deep this seed already is
        root->ownerId = id;

        // Either pushes root's children as the first frame, or fully
        // handles root as a dead end/winning leaf and releases it itself.
        tryExpand(root);
        runStack();
    }

    // Drives the DFS stack to completion (this seed's whole subtree
    // exhausted with no win) or until a solution is found anywhere.
    void runStack() {
        while (!stack.empty()) {
            if (globals.solutionFound.load(std::memory_order_relaxed)) {
                // Someone found it (possibly us, possibly another worker).
                // Leave whatever's left on our stack exactly as-is: if the
                // win is ours, our ancestors must stay alive for
                // reconstruction; if it's someone else's, the process is
                // about to exit anyway and there's nothing to gain by
                // unwinding cleanly here. See exploreFromSeed's win-path
                // comment in tryExpand for the matching reasoning.
                return;
            }
            Frame& top = stack.back();
            if (top.remaining.empty()) {
                // Every child of top.parent has been fully explored with
                // no win found beneath it -- release it and unwind.
                Chain single = Chain::single(top.parent);
                localPool.releaseChain(single);
                stack.pop_back();
                continue;
            }
            JobNode* next = top.remaining.head;
            top.remaining.head = next->next;
            if (!top.remaining.head) top.remaining.tail = nullptr;
            --top.remaining.count;
            next->next = nullptr;

            // Pushes a new frame for `next` if it has live children left
            // to explore; otherwise fully handles/releases `next` itself.
            // Either way, the next loop iteration does the right thing:
            // drill into the new frame, or keep consuming `top.remaining`.
            tryExpand(next);
        }
    }

    // Attempts to expand `parent` into its children via one CUDA
    // round-trip. Returns true and pushes a new frame if `parent` has at
    // least one live, unexplored child left to drive the DFS deeper into.
    // Returns false if `parent` turned out to be a dead end (already
    // released) or the winning leaf was found among its children (parent
    // deliberately left un-released, since it's now part of the ancestor
    // chain the caller needs for path reconstruction).
    bool tryExpand(JobNode* parent) {
        Chain outgoing, results;
        size_t nodesSubmitted = 0;

        for (Move move : candidateMoves(parent->state)) {
            JobNode* child = localPool.alloc();
            child->parent = parent;
            child->level = parent->level + 1;
            child->ownerId = id;
            applyMove(child->state, parent->state, move);
            outgoing.pushBack(child);

            if (outgoing.count == FLUSH_THRESHOLD) {
                nodesSubmitted += outgoing.count;
                dispatcher.submit(id, outgoing.takeAll());
                // Drain opportunistically so our own uncollected-results
                // backlog never grows unbounded relative to the
                // dispatcher's fixed results-inbox capacity (see
                // Dispatcher::tryCollect for why this matters).
                Chain chunk;
                while (dispatcher.tryCollect(id, chunk)) {
                    results.append(chunk);
                }
            }
        }
        if (!outgoing.empty()) {
            nodesSubmitted += outgoing.count;
            dispatcher.submit(id, outgoing.takeAll());
        }
        // Completion is tracked by total NODE count, not by how many
        // submit()/collect() calls happened: the dispatcher may merge
        // several of our submitted chunks into a single result chain
        // whenever more than one of our chunks becomes ready within the
        // same runOnce() round (very likely once a producer has a large
        // backlog) — so per-call chunk counts can never be relied on to
        // match up 1:1, only total node counts can.
        while (results.count < nodesSubmitted) {
            Chain chunk;
            dispatcher.collect(id, chunk); // blocking for the remainder
            results.append(chunk);
        }

        Chain dead, survivors;
        bool wonHere = false;
        JobNode* n = results.head;
        while (n) {
            JobNode* next = n->next;
            n->next = nullptr;
            if (allBugsEaten(n->state)) {
                globals.solutionLeaf = n;
                globals.solutionFound.store(true, std::memory_order_relaxed);
                wonHere = true;
                // Wake anyone (master or a sibling worker) currently
                // blocked in seeds.push()/pop() -- e.g. master mid-push on
                // a full queue while this worker's run() loop is about to
                // exit without ever calling pop() again now that
                // solutionFound is true. Without this, master could block
                // forever with nothing left to drain the queue.
                seeds.abort();
                // Anything else discovered in this same batch no longer
                // matters -- it'll land in `dead` below and get released.
            } else if (isAlive(n->state) && n->level < MAX_DEPTH) {
                survivors.pushBack(n);
            } else {
                dead.pushBack(n); // off-board, or alive but out of moves
            }
            n = next;
        }
        localPool.releaseChain(dead); // O(1) regardless of dead count

        if (wonHere) {
            // These were about to become the next frame, but the search
            // is over -- release them too. `parent` (and everything above
            // it) must stay intact for reconstruction, so it's the one
            // thing here we deliberately do NOT release.
            localPool.releaseChain(survivors);
            return false;
        }

        if (survivors.empty()) {
            // parent is a fully dead end: no legal move keeps this
            // lineage alive. Release it immediately rather than pushing
            // an empty frame just to pop it again next iteration.
            Chain single = Chain::single(parent);
            localPool.releaseChain(single);
            return false;
        }

        stack.push_back(Frame{parent, survivors});
        return true;
    }

    static bool isAlive(const JobState& s) { return s.offBoard == 0; }

    size_t id;
    Dispatcher& dispatcher;
    ThreadLocalPool localPool;
    SeedQueue<32>& seeds;
    SearchGlobals& globals;
    std::vector<Frame> stack;
};
