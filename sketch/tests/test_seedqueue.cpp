// Test for SeedQueue: single-producer (master) / multi-consumer (workers)
// bounded blocking queue. Verifies every seed is delivered to exactly one
// consumer, none lost or duplicated, and that pop() correctly unblocks and
// returns false once finished() has been called and the queue is drained.
#include "../SeedQueue.hpp"
#include "test_util.hpp"
#include <thread>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>

int main() {
    constexpr size_t CAP = 16; // small, to force push() to block
    constexpr int NUM_SEEDS = 50000;
    constexpr int NUM_CONSUMERS = 6;

    SeedQueue<CAP> queue;
    std::vector<std::vector<int>> consumed(NUM_CONSUMERS);

    std::thread producer([&] {
        for (int i = 0; i < NUM_SEEDS; ++i) {
            Seed s{};
            std::memcpy(s.state.boardBytes, &i, sizeof(i)); // tag with sequence id
            queue.push(s);
        }
        queue.finished();
    });

    std::vector<std::thread> consumers;
    for (int c = 0; c < NUM_CONSUMERS; ++c) {
        consumers.emplace_back([&, c] {
            Seed s;
            while (queue.pop(s)) {
                int tag;
                std::memcpy(&tag, s.state.boardBytes, sizeof(tag));
                consumed[c].push_back(tag);
            }
        });
    }

    producer.join();
    for (auto& t : consumers) t.join();

    std::vector<int> all;
    for (auto& v : consumed) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());

    CHECK(all.size() == static_cast<size_t>(NUM_SEEDS));
    bool complete = true;
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i] != static_cast<int>(i)) {
            complete = false;
            break;
        }
    }
    CHECK(complete);
    std::printf("SeedQueue: %d/%d seeds delivered exactly once, %s\n", (int)all.size(), NUM_SEEDS,
                complete ? "no gaps/dupes" : "MISMATCH");

    REPORT();
}
