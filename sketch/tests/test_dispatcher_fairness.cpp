// Test/benchmark for Dispatcher's round-robin fairness: a producer with a
// huge backlog (simulating a worker deep in an 810k-leaf level) must not
// be able to starve producers with small backlogs (simulating a worker
// that just grabbed a fresh seed) — small producers should finish quickly
// rather than waiting for the big one to fully drain (the old, naive
// FIFO-drain-one-producer-then-the-next behaviour this replaces).
#include "../Dispatcher.hpp"
#include "../NodePool.hpp"
#include "test_util.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdio>

// Fake CUDA kernel: no real board logic here, just touches memory (so the
// compiler can't elide the batch) and tracks how many jobs have been
// "processed" per producer, via the ownerId-independent state payload.
static std::atomic<long> g_batchesRun{0};
void launchCudaBatch(JobState* states, size_t n) {
    for (size_t i = 0; i < n; ++i) states[i].bugsEatenMask = 0; // trivial touch
    g_batchesRun.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::microseconds(50)); // pretend GPU latency
}

constexpr size_t CHUNK = 200;

struct ProducerResult {
    long finishedAtBatchIndex = -1;
};

static void runFakeProducer(size_t producerId, size_t totalNodes, Dispatcher& dispatcher,
                            NodePool& pool, ProducerResult& result) {
    ThreadLocalPool local(pool, CHUNK);
    Chain outgoing;
    size_t nodesSubmitted = 0;
    Chain gathered;
    for (size_t i = 0; i < totalNodes; ++i) {
        JobNode* n = local.alloc();
        n->ownerId = static_cast<uint32_t>(producerId);
        n->level = 7;
        n->parent = nullptr;
        outgoing.pushBack(n);
        if (outgoing.count == CHUNK) {
            nodesSubmitted += outgoing.count;
            dispatcher.submit(producerId, outgoing.takeAll());
            // Interleave collection with submission — see
            // Dispatcher::tryCollect for why this is required to avoid
            // deadlocking the single dispatcher thread on a large backlog.
            Chain chunk;
            while (dispatcher.tryCollect(producerId, chunk)) {
                gathered.append(chunk);
            }
        }
    }
    if (!outgoing.empty()) {
        nodesSubmitted += outgoing.count;
        dispatcher.submit(producerId, outgoing.takeAll());
    }

    // Completion is tracked by total NODE count, not by counting
    // submit()/collect() calls: the dispatcher may merge several of our
    // submitted chunks into a single result chain whenever more than one
    // becomes ready within the same runOnce() round (near-guaranteed for
    // a producer with a large backlog, like the "big" producer here) --
    // so per-call chunk counts never reliably match up 1:1, only totals do.
    while (gathered.count < nodesSubmitted) {
        Chain chunk;
        dispatcher.collect(producerId, chunk);
        gathered.append(chunk);
    }
    result.finishedAtBatchIndex = g_batchesRun.load(std::memory_order_relaxed);
    local.releaseChain(gathered);
}

int main() {
    constexpr size_t NUM_SMALL_PRODUCERS = 4;
    constexpr size_t BIG_PRODUCER_ID = NUM_SMALL_PRODUCERS; // producer 4
    constexpr size_t BIG_TOTAL = 200000;                    // e.g. a worker deep in level 7
    constexpr size_t SMALL_TOTAL = 2000;                    // e.g. a worker on a fresh seed

    NodePool pool(BIG_TOTAL + NUM_SMALL_PRODUCERS * SMALL_TOTAL + 1000);
    Dispatcher dispatcher(NUM_SMALL_PRODUCERS + 1);

    std::atomic<bool> stop{false};
    std::thread dispatcherThread([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            if (!dispatcher.runOnce()) std::this_thread::yield();
        }
    });

    std::vector<ProducerResult> results(NUM_SMALL_PRODUCERS + 1);
    std::vector<std::thread> producers;
    for (size_t i = 0; i < NUM_SMALL_PRODUCERS; ++i) {
        producers.emplace_back(runFakeProducer, i, SMALL_TOTAL, std::ref(dispatcher),
                               std::ref(pool), std::ref(results[i]));
    }
    producers.emplace_back(runFakeProducer, BIG_PRODUCER_ID, BIG_TOTAL, std::ref(dispatcher),
                           std::ref(pool), std::ref(results[BIG_PRODUCER_ID]));

    for (auto& t : producers) t.join();
    stop.store(true, std::memory_order_relaxed);
    dispatcherThread.join();

    long bigFinish = results[BIG_PRODUCER_ID].finishedAtBatchIndex;
    std::printf("Big producer (%zu nodes) finished at batch #%ld\n", BIG_TOTAL, bigFinish);
    long maxSmallFinish = 0;
    for (size_t i = 0; i < NUM_SMALL_PRODUCERS; ++i) {
        std::printf("Small producer %zu (%zu nodes) finished at batch #%ld\n", i, SMALL_TOTAL,
                    results[i].finishedAtBatchIndex);
        maxSmallFinish = std::max(maxSmallFinish, results[i].finishedAtBatchIndex);
    }

    // The key fairness property: small producers must not have to wait for
    // the big backlog to fully drain. Big producer needs ~BIG_TOTAL/1000
    // batches; if fairness holds, small producers finish in a small
    // fraction of that, not near the end.
    CHECK(maxSmallFinish < bigFinish / 2);
    CHECK(pool.inUseCount() == 0); // everything returned, no leaks

    REPORT();
}
