#pragma once

#include "Chain.hpp"
#include "Dispatcher.hpp"
#include "NodePool.hpp"
#include "SearchInstrumentation.hpp"
#include "SeedQueue.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <vector>

constexpr size_t FLUSH_THRESHOLD = 200;
constexpr int MAX_SUPPORTED_DEPTH = 7;
constexpr int MAX_DEPTH = MAX_SUPPORTED_DEPTH; // compatibility for simulations/tests

struct SearchGlobals {
    std::atomic<bool> solutionFound{false};
    JobNode* solutionLeaf = nullptr;
    std::mutex solutionMutex;

    bool publishSolution(JobNode* leaf) {
        std::lock_guard<std::mutex> lock(solutionMutex);
        if (solutionFound.load(std::memory_order_relaxed)) return false;
        solutionLeaf = leaf;
        solutionFound.store(true, std::memory_order_release);
        return true;
    }
};

struct Move {
    uint8_t insertPoint;
    uint8_t orientation;
};

std::vector<Move> candidateMoves(const JobState& parent);
void applyMove(JobState& out, const JobState& parent, Move move);
bool allBugsEaten(const JobState& state);

struct WorkerSchedulerConfig {
    size_t maxJobsInFlight = 1000;
    size_t resumeJobsInFlight = 750;
    size_t maxResidentNodes = 5000;
    // Conservative branching used to reserve enough memory to drive one
    // deepest branch to terminal depth even when the rest of the budget is
    // occupied by ready/in-flight nodes.
    size_t escapeBranching = 150;
    uint8_t maxDepth = MAX_SUPPORTED_DEPTH;
    uint8_t seedDepth = 4;
};

struct WorkerSchedulerStats {
    uint64_t expansionsSubmitted = 0;
    uint64_t jobsSubmitted = 0;
    uint64_t resultsReceived = 0;
    uint64_t completedSeeds = 0;
    uint64_t throttleTransitions = 0;
    size_t peakJobsInFlight = 0;
    size_t peakReadyNodes = 0;
    size_t peakResidentNodes = 0;
};

// Bounded asynchronous, depth-priority exhaustive worker.
//
// Unlike the original strict DFS implementation, this worker may have several
// parent expansions in flight simultaneously. Evaluated survivors are kept in
// per-depth FIFO queues; the deepest ready depth is always expanded first.
// Submission pauses at maxJobsInFlight and resumes below the lower hysteresis
// threshold. A strict resident-node budget plus an escape reserve prevents the
// bounded frontier from exhausting memory.
//
// No scheduling threshold prunes. Completion propagates toward the seed root
// only when an expanded node's final child subtree completes negatively.
class Worker {
public:
    Worker(size_t id, Dispatcher& dispatcher, NodePool& pool, SeedQueue<32>& seeds,
           SearchGlobals& globals, SearchInstrumentationSink* instrumentation = nullptr,
           WorkerSchedulerConfig config = {})
        : id(id), dispatcher(dispatcher), localPool(pool, FLUSH_THRESHOLD), seeds(seeds),
          globals(globals), instrumentation(instrumentation), config(config) {
        validateConfig();
    }

    void run() {
        Seed seed;
        while (!globals.solutionFound.load(std::memory_order_relaxed) && seeds.pop(seed))
            exploreFromSeed(seed);
    }

    const WorkerSchedulerStats& schedulerStats() const { return stats; }

private:
    void validateConfig() const {
        if (config.maxJobsInFlight == 0)
            throw std::invalid_argument("maxJobsInFlight must be positive");
        if (config.resumeJobsInFlight >= config.maxJobsInFlight)
            throw std::invalid_argument("resumeJobsInFlight must be below maxJobsInFlight");
        if (config.escapeBranching == 0)
            throw std::invalid_argument("escapeBranching must be positive");
        if (config.maxDepth > MAX_SUPPORTED_DEPTH || config.seedDepth > config.maxDepth)
            throw std::invalid_argument("invalid runtime search depth");
        const size_t minimum = config.escapeBranching * (config.maxDepth - config.seedDepth);
        if (config.maxResidentNodes <= minimum)
            throw std::invalid_argument("maxResidentNodes is too small for the escape reserve");
    }

    void initializeNode(JobNode* node, JobNode* parent, uint32_t level) {
        node->parent = parent;
        node->level = level;
        node->ownerId = static_cast<uint32_t>(id);
        node->outstandingChildren = 0;
        node->expansionComplete = false;
        ++residentNodes;
        stats.peakResidentNodes = std::max(stats.peakResidentNodes, residentNodes);
    }

    void exploreFromSeed(const Seed& seed) {
        clearSchedulerState();
        activeSeedJobs.fill(0);

        JobNode* root = localPool.alloc();
        if (!root) throw std::runtime_error("NodePool exhausted allocating worker seed root");
        root->state = seed.state;
        initializeNode(root, nullptr, seed.depth);
        activeRoot = root;
        pushReady(root);

        while (activeRoot && !globals.solutionFound.load(std::memory_order_relaxed)) {
            drainAvailableResults();
            updateThrottle();

            bool submitted = false;
            while (!submissionPaused && !globals.solutionFound.load(std::memory_order_relaxed)) {
                JobNode* node = peekDeepestReady();
                if (!node) break;

                std::vector<Move> moves = candidateMoves(node->state);
                if (!canExpand(*node, moves.size())) {
                    // In-flight pressure is recoverable by collecting. If no
                    // work can return, the configured resident budget cannot
                    // satisfy its promised escape reserve.
                    submissionPaused = jobsInFlight > 0;
                    if (!submissionPaused)
                        throw std::runtime_error(
                            "resident-node budget cannot advance deepest ready branch");
                    break;
                }

                popDeepestReady();
                expandNode(node, moves);
                submitted = true;
                drainAvailableResults();
                updateThrottle();
            }

            if (!activeRoot || globals.solutionFound.load(std::memory_order_relaxed)) break;
            if (jobsInFlight > 0 && (submissionPaused || !submitted || !hasReady())) {
                Chain results;
                if (!dispatcher.collectCancellable(id, results, globals.solutionFound)) break;
                processResults(results);
                updateThrottle();
            } else if (!hasReady() && jobsInFlight == 0) {
                throw std::logic_error("worker seed has no ready or in-flight work but is incomplete");
            }
        }

        // Early solution/abort stops the whole search. This producer explicitly
        // abandons later results so postprocessing can drain without blocking
        // on an inbox nobody will consume. Nodes are not released or marked
        // negative; they remain arena-resident until process teardown.
        if (globals.solutionFound.load(std::memory_order_relaxed)) {
            dispatcher.abandonProducer(id);
            return;
        }

        if (!globals.solutionFound.load(std::memory_order_relaxed)) {
            if (residentNodes != 0 || readyNodes != 0 || jobsInFlight != 0)
                throw std::logic_error("worker completed seed with retained scheduler state");
            ++stats.completedSeeds;
            if (instrumentation) instrumentation->recordCompletedSeed(activeSeedJobs);
        }
    }

    bool canExpand(const JobNode& parent, size_t candidates) const {
        if (jobsInFlight + candidates > config.maxJobsInFlight && jobsInFlight > 0) return false;
        const size_t childDepth = parent.level + 1;
        const size_t remainingExpansions =
            childDepth < config.maxDepth ? static_cast<size_t>(config.maxDepth - childDepth) : 0;
        const size_t escapeReserve = remainingExpansions * config.escapeBranching;
        return residentNodes + candidates + escapeReserve <= config.maxResidentNodes;
    }

    void expandNode(JobNode* parent, const std::vector<Move>& moves) {
        if (moves.empty()) {
            parent->expansionComplete = true;
            completeSubtree(parent);
            return;
        }

        parent->expansionComplete = false;

        Chain outgoing;
        for (Move move : moves) {
            JobNode* child = localPool.alloc();
            if (!child) throw std::runtime_error("NodePool exhausted despite worker resident budget");
            initializeNode(child, parent, parent->level + 1);
            applyMove(child->state, parent->state, move);
            ++parent->outstandingChildren;
            outgoing.pushBack(child);
            ++jobsInFlight;
            ++stats.jobsSubmitted;
            if (child->level < activeSeedJobs.size()) ++activeSeedJobs[child->level];

            if (outgoing.count == FLUSH_THRESHOLD) {
                dispatcher.submit(id, outgoing.takeAll());
                drainAvailableResults();
            }
        }
        if (!outgoing.empty()) dispatcher.submit(id, outgoing.takeAll());
        parent->expansionComplete = true;

        ++stats.expansionsSubmitted;
        stats.peakJobsInFlight = std::max(stats.peakJobsInFlight, jobsInFlight);
        if (instrumentation)
            instrumentation->recordExpansion(parent->level, moves.size(), false);
    }

    void drainAvailableResults() {
        Chain results;
        while (dispatcher.tryCollect(id, results)) {
            processResults(results);
            results = {};
        }
    }

    void processResults(Chain& results) {
        JobNode* node = results.head;
        results = {};
        while (node) {
            JobNode* next = node->next;
            node->next = nullptr;
            if (jobsInFlight == 0) throw std::logic_error("received more jobs than were in flight");
            --jobsInFlight;
            ++stats.resultsReceived;

            if (allBugsEaten(node->state)) {
                globals.publishSolution(node);
                seeds.abort();
            } else if (node->state.offBoard == 0 && node->level < config.maxDepth) {
                pushReady(node);
            } else {
                node->expansionComplete = true; // evaluated negative/terminal leaf
                completeSubtree(node);
            }
            node = next;
        }
    }

    void completeSubtree(JobNode* node) {
        while (node) {
            if (!node->expansionComplete && node->level < config.maxDepth)
                throw std::logic_error("attempted to prune an unexpanded live node");
            if (node->outstandingChildren != 0)
                throw std::logic_error("attempted to prune a node with unfinished children");

            JobNode* parent = node->parent;
            bool wasRoot = node == activeRoot;
            localPool.releaseChain(Chain::single(node));
            if (residentNodes == 0) throw std::logic_error("resident-node accounting underflow");
            --residentNodes;
            if (wasRoot) activeRoot = nullptr;
            if (!parent) return;
            if (parent->outstandingChildren == 0)
                throw std::logic_error("parent child-completion accounting underflow");
            --parent->outstandingChildren;
            if (!parent->expansionComplete || parent->outstandingChildren != 0) return;
            node = parent;
        }
    }

    void pushReady(JobNode* node) {
        readyByDepth[node->level].push_back(node);
        ++readyNodes;
        stats.peakReadyNodes = std::max(stats.peakReadyNodes, readyNodes);
    }

    JobNode* peekDeepestReady() {
        for (int depth = config.maxDepth - 1; depth >= 0; --depth)
            if (!readyByDepth[depth].empty()) return readyByDepth[depth].front();
        return nullptr;
    }

    void popDeepestReady() {
        for (int depth = config.maxDepth - 1; depth >= 0; --depth) {
            if (readyByDepth[depth].empty()) continue;
            readyByDepth[depth].pop_front();
            --readyNodes;
            return;
        }
        throw std::logic_error("popDeepestReady called with no ready node");
    }

    bool hasReady() const { return readyNodes != 0; }

    void updateThrottle() {
        bool old = submissionPaused;
        if (jobsInFlight >= config.maxJobsInFlight)
            submissionPaused = true;
        else if (jobsInFlight < config.resumeJobsInFlight)
            submissionPaused = false;
        if (old != submissionPaused) ++stats.throttleTransitions;
    }

    void clearSchedulerState() {
        for (auto& queue : readyByDepth) queue.clear();
        readyNodes = 0;
        jobsInFlight = 0;
        residentNodes = 0;
        submissionPaused = false;
        activeRoot = nullptr;
    }

    size_t id;
    Dispatcher& dispatcher;
    ThreadLocalPool localPool;
    SeedQueue<32>& seeds;
    SearchGlobals& globals;
    SearchInstrumentationSink* instrumentation;
    WorkerSchedulerConfig config;
    WorkerSchedulerStats stats;

    std::array<std::deque<JobNode*>, MAX_SUPPORTED_DEPTH + 1> readyByDepth;
    std::array<uint64_t, INSTRUMENTED_DEPTHS> activeSeedJobs{};
    size_t readyNodes = 0;
    size_t jobsInFlight = 0;
    size_t residentNodes = 0;
    bool submissionPaused = false;
    JobNode* activeRoot = nullptr;
};
