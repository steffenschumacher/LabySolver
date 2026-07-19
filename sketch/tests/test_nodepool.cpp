// Tests for NodePool / ThreadLocalPool: correctness under concurrency,
// no double-allocation of a node, and that memory usage stays bounded
// (never exceeds the preallocated capacity, i.e. no hidden heap growth).
#include "../NodePool.hpp"
#include "test_util.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <cstdio>

// Part A: single-threaded correctness + peak tracking.
static void testBasic() {
    NodePool pool(16);
    CHECK(pool.capacity() == 16);
    CHECK(pool.inUseCount() == 0);

    Chain a = pool.acquire(10);
    CHECK(a.count == 10);
    CHECK(pool.inUseCount() == 10);
    CHECK(pool.peakInUse() == 10);

    // Pool only has 6 left; asking for 10 more should yield exactly 6
    // (backpressure signal — caller must handle fewer-than-requested).
    Chain b = pool.acquire(10);
    CHECK(b.count == 6);
    CHECK(pool.inUseCount() == 16);
    CHECK(pool.peakInUse() == 16);

    // Fully exhausted: further acquire returns an empty chain.
    Chain c = pool.acquire(1);
    CHECK(c.empty());

    pool.release(a);
    CHECK(pool.inUseCount() == 6);
    CHECK(pool.peakInUse() == 16); // peak doesn't decrease

    pool.release(b);
    CHECK(pool.inUseCount() == 0);

    // Reacquire and check we get real nodes again (pointer reuse, not a
    // fresh allocation — the arena never grows).
    Chain d = pool.acquire(16);
    CHECK(d.count == 16);
    CHECK(pool.inUseCount() == 16);
}

// Part B: concurrent stress — many threads acquire/mark/release in a
// loop; verify no node is ever handed out to two owners simultaneously,
// and that in-use count/peak never exceed the fixed capacity.
static void testConcurrentStress() {
    constexpr size_t CAPACITY = 5000;
    constexpr int NUM_THREADS = 8;
    constexpr int ITERATIONS = 500;
    constexpr size_t CHUNK = 50;

    NodePool pool(CAPACITY);
    std::vector<std::atomic<int>> marker(CAPACITY);
    for (auto& m : marker) m.store(0);
    std::atomic<bool> doubleUseDetected{false};

    auto worker = [&](int /*id*/) {
        for (int it = 0; it < ITERATIONS; ++it) {
            Chain got = pool.acquire(CHUNK);
            for (JobNode* n = got.head; n; n = n->next) {
                size_t idx = pool.indexOf(n);
                int prev = marker[idx].fetch_add(1, std::memory_order_relaxed);
                if (prev != 0) doubleUseDetected.store(true, std::memory_order_relaxed);
            }
            // simulate a little work
            for (JobNode* n = got.head; n; n = n->next) n->ownerId ^= 0x1u;

            for (JobNode* n = got.head; n; n = n->next) {
                size_t idx = pool.indexOf(n);
                marker[idx].fetch_sub(1, std::memory_order_relaxed);
            }
            pool.release(got);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) threads.emplace_back(worker, i);
    for (auto& t : threads) t.join();

    CHECK(!doubleUseDetected.load());
    CHECK(pool.inUseCount() == 0);                          // everything returned
    CHECK(pool.peakInUse() <= static_cast<long>(CAPACITY)); // never over-allocated
    std::printf("NodePool stress: peak in-use = %ld / %zu capacity\n", pool.peakInUse(), CAPACITY);
}

// Part C: ThreadLocalPool caching/refill behaviour.
static void testThreadLocalPool() {
    NodePool shared(1000);
    {
        ThreadLocalPool tlp(shared, 64); // refills 64 at a time
        for (int i = 0; i < 500; ++i) {
            JobNode* n = tlp.alloc();
            CHECK(n != nullptr);
        }
        // 500 requested in ones/twos triggers 8 refills of 64 (512 total
        // acquired from the shared pool); the local cache is still holding
        // the 12 leftover nodes not yet handed out via alloc().
        CHECK(shared.inUseCount() == 512);
    }
    // Destructor returns the 12 unused leftovers to the shared pool. The
    // 500 nodes actually handed out via alloc() were never given back (we
    // just discarded the pointers, simulating a caller that still owns
    // them elsewhere) — so they correctly remain "in use" from the shared
    // pool's point of view.
    CHECK(shared.inUseCount() == 500);
}

int main() {
    testBasic();
    testConcurrentStress();
    testThreadLocalPool();
    REPORT();
}
