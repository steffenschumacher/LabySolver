#pragma once
#include "Chain.hpp"
#include <array>
#include <atomic>

// ---------------------------------------------------------------------------
// ChainInbox: single-producer / single-consumer ring buffer whose elements
// are whole Chains (head/tail/count descriptors, ~24 bytes), not individual
// jobs. One producer thread pushes ~FLUSH_THRESHOLD-sized chunks; the
// dispatcher thread pops them. Lock-free since it's strictly SPSC.
//
// Capacity is small on purpose (this holds *chain descriptors*, not nodes —
// the nodes themselves live in the NodePool arena). If push() ever returns
// false in practice, the dispatcher isn't draining fast enough relative to
// the producer; increase Capacity or investigate dispatcher throughput.
// ---------------------------------------------------------------------------
template <size_t Capacity> class ChainInbox {
public:
    // Producer side. Returns false if full — caller should back off
    // (spin/yield) briefly and retry.
    bool push(Chain chain) {
        size_t h = head.load(std::memory_order_relaxed);
        size_t nextH = (h + 1) % Capacity;
        if (nextH == tail.load(std::memory_order_acquire)) return false; // full
        slots[h] = chain;
        head.store(nextH, std::memory_order_release);
        return true;
    }

    // Consumer side (dispatcher thread only).
    bool pop(Chain& out) {
        size_t t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire)) return false; // empty
        out = slots[t];
        tail.store((t + 1) % Capacity, std::memory_order_release);
        return true;
    }

private:
    std::array<Chain, Capacity> slots{};
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};
};
