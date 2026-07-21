#pragma once
#include "Worker.hpp" // reuses Move / candidateMoves / applyMove / allBugsEaten / SearchGlobals
#include "SeedQueue.hpp"
#include "NodePool.hpp"
#include "Dispatcher.hpp"

constexpr size_t MASTER_ID = 0; // producer id 0 reserved for master
// Master owns moves 1..MASTER_DEPTH; workers own the rest
// (MASTER_DEPTH+1..MAX_DEPTH). Set to 4 so workers start from move 5,
// i.e. own moves 5, 6 & 7 (3 levels) via their own DFS.
constexpr int MASTER_DEPTH = 4;

struct MasterSearchConfig {
    uint8_t maxDepth = MAX_SUPPORTED_DEPTH;
    uint8_t seedDepth = MASTER_DEPTH;
};

// ---------------------------------------------------------------------------
// Master: the *same* explicit-stack DFS as Worker (see Worker.hpp's class
// comment for why DFS instead of BFS -- it applies here just as much: with
// branching ~150 once the ladybug's pre-insertion reposition choice is
// folded in, a full breadth-first sweep to depth 4 would be ~150^4 =~ 500M
// nodes, exactly the blowup this whole redesign avoids). The only
// difference from Worker: instead of bottoming out at MAX_DEPTH and
// checking for a win, Master bottoms out at MASTER_DEPTH and hands
// surviving nodes off to the workers as Seeds instead of expanding them
// further itself. Runs once, start to finish, then retires.
// ---------------------------------------------------------------------------
class Master {
public:
    Master(Dispatcher& dispatcher, NodePool& pool, SeedQueue<32>& seedQueue, SearchGlobals& globals,
           const JobState& initialBoard, SearchInstrumentation* instrumentation = nullptr,
           MasterSearchConfig config = {}, MasterSeedPersistence* persistence = nullptr)
        : dispatcher(dispatcher), localPool(pool, FLUSH_THRESHOLD), seedQueue(seedQueue),
          globals(globals), initialBoard(initialBoard), instrumentation(instrumentation), config(config),
          persistence(persistence) {
        if (config.maxDepth > MAX_SUPPORTED_DEPTH) throw std::invalid_argument("invalid max depth");
        if (config.seedDepth > config.maxDepth) config.seedDepth = config.maxDepth;
    }

    void run() {
        JobNode* root = localPool.alloc();
        root->state = initialBoard;
        root->parent = nullptr;
        root->level = 0;
        root->ownerId = MASTER_ID;

        tryExpand(root);
        runStack();

        if (instrumentation && !globals.solutionFound.load(std::memory_order_relaxed))
            instrumentation->markMasterFinished();
        if (persistence && !globals.solutionFound.load(std::memory_order_relaxed))
            persistence->markMasterFinished();
        seedQueue.finished(); // unblock any workers waiting on pop(), regardless of outcome
    }

private:
    struct Frame {
        JobNode* parent;
        Chain remaining;
    };

    void runStack() {
        while (!stack.empty()) {
            if (globals.solutionFound.load(std::memory_order_relaxed)) return;
            Frame& top = stack.back();
            if (top.remaining.empty()) {
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

            if (next->level >= config.maxDepth) {
                Chain terminal = Chain::single(next);
                localPool.releaseChain(terminal);
                continue;
            }
            if (next->level >= config.seedDepth) {
                // Reached the master/worker boundary alive (a win at this
                // depth was already caught and handled inside the parent's
                // tryExpand call, same as Worker) -- hand it to the
                // workers as a seed instead of expanding it ourselves.
                emitSeed(next);
                continue;
            }
            tryExpand(next);
        }
    }

    // Copies out everything a worker needs (board state + the moves that
    // produced it, walked back via parent pointers) into a Seed, then
    // releases `n` itself immediately -- its ancestors are untouched and
    // get released normally, later, when their own frame is exhausted.
    void emitSeed(JobNode* n) {
        Seed seed{};
        seed.state = n->state;
        seed.depth = static_cast<uint8_t>(n->level);
        JobNode* cur = n;
        for (int i = static_cast<int>(n->level) - 1; i >= 0 && cur; --i, cur = cur->parent) {
            seed.moves[i] = {cur->state.insertPoint, cur->state.orientation};
        }
        const uint64_t ordinal = ++generatedSeedOrdinal;
        seed.id = ordinal;
        bool shouldEnqueue = !persistence || persistence->registerGeneratedSeed(ordinal, seed);
        bool emitted = shouldEnqueue && seedQueue.push(seed);
        if (instrumentation && emitted) instrumentation->recordSeedEmitted();

        Chain single = Chain::single(n);
        localPool.releaseChain(single);
    }

    // Same shape as Worker::tryExpand, minus the MAX_DEPTH survivor cutoff
    // (Master's runStack already decides "keep expanding vs. hand off as a
    // seed" by depth *before* calling tryExpand, so tryExpand itself just
    // needs to know alive vs. dead vs. won).
    bool tryExpand(JobNode* parent) {
        Chain outgoing, results;
        size_t nodesSubmitted = 0;

        for (Move move : candidateMoves(parent->state)) {
            JobNode* child = localPool.alloc();
            child->parent = parent;
            child->level = parent->level + 1;
            child->ownerId = MASTER_ID;
            applyMove(child->state, parent->state, move);
            outgoing.pushBack(child);

            if (outgoing.count == FLUSH_THRESHOLD) {
                nodesSubmitted += outgoing.count;
                dispatcher.submit(MASTER_ID, outgoing.takeAll());
                Chain chunk;
                while (dispatcher.tryCollect(MASTER_ID, chunk)) {
                    results.append(chunk);
                }
            }
        }
        if (!outgoing.empty()) {
            nodesSubmitted += outgoing.count;
            dispatcher.submit(MASTER_ID, outgoing.takeAll());
        }
        // See Worker::tryExpand's comment: completion must be tracked by
        // total node count, not by submit()/collect() call counts, since
        // the dispatcher may merge multiple submitted chunks into one
        // result chain within a single prepared dispatcher batch.
        while (results.count < nodesSubmitted) {
            Chain chunk;
            dispatcher.collect(MASTER_ID, chunk);
            results.append(chunk);
        }
        Chain dead, survivors;
        bool wonHere = false;
        JobNode* n = results.head;
        while (n) {
            JobNode* next = n->next;
            n->next = nullptr;
            if (allBugsEaten(n->state)) {
                globals.publishSolution(n);
                wonHere = true;
                // Wake any thread currently blocked in seedQueue.push()/pop()
                // so it doesn't wait forever on an event that will never
                // come now that a solution has been found elsewhere.
                seedQueue.abort();
            } else if (n->state.offBoard == 0) {
                survivors.pushBack(n);
            } else {
                dead.pushBack(n);
            }
            n = next;
        }
        if (instrumentation) {
            instrumentation->recordExpansion(parent->level, nodesSubmitted, true);
            instrumentation->recordMasterSurvivors(parent->level + 1, survivors.count);
        }
        localPool.releaseChain(dead);

        if (wonHere) {
            localPool.releaseChain(survivors);
            return false;
        }
        if (survivors.empty()) {
            Chain single = Chain::single(parent);
            localPool.releaseChain(single);
            return false;
        }
        stack.push_back(Frame{parent, survivors});
        return true;
    }

    Dispatcher& dispatcher;
    ThreadLocalPool localPool;
    SeedQueue<32>& seedQueue;
    SearchGlobals& globals;
    JobState initialBoard;
    SearchInstrumentation* instrumentation;
    MasterSearchConfig config;
    MasterSeedPersistence* persistence;
    uint64_t generatedSeedOrdinal = 0;
    std::vector<Frame> stack;
};
