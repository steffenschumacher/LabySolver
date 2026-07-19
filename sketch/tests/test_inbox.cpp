// Test for ChainInbox: SPSC ring buffer of Chain descriptors. Verifies
// FIFO ordering under real producer/consumer contention (including the
// full-buffer backoff path), using a small capacity to force it.
#include "../Inbox.hpp"
#include "test_util.hpp"
#include <thread>
#include <vector>
#include <cstdio>

int main() {
    constexpr size_t CAP = 8; // small on purpose, to force "full" often
    constexpr int NUM_ITEMS = 200000;

    ChainInbox<CAP> inbox;
    std::vector<size_t> received;
    received.reserve(NUM_ITEMS);

    std::thread producer([&] {
        for (int i = 0; i < NUM_ITEMS; ++i) {
            Chain c; // synthetic descriptor; count field doubles as a sequence tag here
            c.count = static_cast<size_t>(i);
            while (!inbox.push(c))
                std::this_thread::yield(); // full -> back off, matches Dispatcher::submit
        }
    });

    std::thread consumer([&] {
        Chain c;
        int got = 0;
        while (got < NUM_ITEMS) {
            if (inbox.pop(c)) {
                received.push_back(c.count);
                ++got;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    CHECK(received.size() == static_cast<size_t>(NUM_ITEMS));
    bool inOrder = true;
    for (size_t i = 0; i < received.size(); ++i) {
        if (received[i] != i) {
            inOrder = false;
            break;
        }
    }
    CHECK(inOrder);
    std::printf("ChainInbox: %d items transferred, FIFO order %s\n", NUM_ITEMS,
                inOrder ? "preserved" : "BROKEN");

    REPORT();
}
