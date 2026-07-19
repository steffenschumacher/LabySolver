#include "../Dispatcher.hpp"
#include "../NodePool.hpp"
#include "test_util.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

static std::atomic<size_t> g_launches{0};
static std::atomic<size_t> g_largestBatch{0};
static std::mutex g_launchMutex;
static std::condition_variable g_launched;

void launchCudaBatch(JobState* states, size_t count) {
    for (size_t i = 0; i < count; ++i) states[i].bugsEatenMask = 0xA;
    g_launches.fetch_add(1, std::memory_order_relaxed);
    size_t previous = g_largestBatch.load(std::memory_order_relaxed);
    while (count > previous &&
           !g_largestBatch.compare_exchange_weak(previous, count, std::memory_order_relaxed)) {
    }
    g_launched.notify_all();
}

int main() {
    Dispatcher dispatcher(1);
    NodePool pool(2000);
    dispatcher.start();

    // A single partial request cannot wait for a full batch. It must cross all
    // three stages after the 2ms preprocessing deadline.
    ThreadLocalPool local(pool, 1);
    JobNode* one = local.alloc();
    one->ownerId = 0;
    one->state.bugsEatenMask = 0;
    auto start = std::chrono::steady_clock::now();
    dispatcher.submit(0, Chain::single(one));
    Chain returned;
    dispatcher.collect(0, returned);
    auto latency = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    CHECK(returned.count == 1);
    CHECK(returned.head->state.bugsEatenMask == 0xA);
    // Generous scheduler allowance around the hard 2ms batching deadline.
    CHECK(latency < 50.0);
    local.releaseChain(returned);

    // A full 1000-job request bypasses the deadline and never exceeds the
    // kernel's batch contract.
    Chain full;
    for (size_t i = 0; i < MAX_BATCH; ++i) {
        JobNode* node = local.alloc();
        node->ownerId = 0;
        full.pushBack(node);
    }
    start = std::chrono::steady_clock::now();
    dispatcher.submit(0, full.takeAll());
    dispatcher.collect(0, returned);
    double fullLatency = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    CHECK(returned.count == MAX_BATCH);
    CHECK(fullLatency < 50.0);
    local.releaseChain(returned);

    dispatcher.stop();
    DispatcherStatsSnapshot stats = dispatcher.stats();
    CHECK(stats.jobsPrepared == MAX_BATCH + 1);
    CHECK(stats.batchesPrepared == 2);
    CHECK(stats.fullBatches == 1);
    CHECK(stats.deadlineFlushes == 1);
    CHECK(g_launches.load(std::memory_order_relaxed) == 2);
    CHECK(g_largestBatch.load(std::memory_order_relaxed) == MAX_BATCH);
    CHECK(pool.inUseCount() == 0);

    std::printf("partial batch end-to-end latency: %.3f ms; full batch: %.3f ms\n", latency,
                fullLatency);
    REPORT();
}
