#pragma once
#include "Chain.hpp"
#include <vector>
#include <mutex>
#include <atomic>

// ---------------------------------------------------------------------------
// NodePool: arena + free-list allocator for JobNode.
//
// Preallocates `capacity` nodes exactly once at construction; alloc()/
// release() never touch the OS allocator again for the lifetime of the run.
//
// Sizing: with Master and Worker both doing explicit-stack DFS (see
// Worker.hpp's class comment), peak concurrent node count is
// O(numProducers x MAX_DEPTH x branching) -- e.g. 11 producers x 7 depth x
// 150 branching =~ 11,550 nodes -- NOT O(branching^depth). Size `capacity`
// to a healthy multiple of that (plus in-flight CUDA batch/staging
// headroom, e.g. MAX_BATCH), not to the full theoretical leaf count; if
// this pool ever runs dry in practice, that means branching turned out
// much higher than assumed, not that a level's worth of leaves is being
// held at once. Running dry just means acquire() returns fewer nodes than
// asked for, which callers should treat as backpressure.
// ---------------------------------------------------------------------------
class NodePool {
public:
    explicit NodePool(size_t capacity) : arena(capacity) {
        for (auto& node : arena) freeChain.pushBack(&node);
    }

    // Bulk-refill: hand out up to `want` nodes as a chain in one locked
    // section, rather than locking per node.
    Chain acquire(size_t want) {
        std::lock_guard<std::mutex> lk(m);
        Chain out;
        while (out.count < want && !freeChain.empty()) {
            JobNode* n = freeChain.head;
            freeChain.head = n->next;
            if (!freeChain.head) freeChain.tail = nullptr;
            --freeChain.count;
            out.pushBack(n);
        }
        if (out.count) {
            long now = inUse.fetch_add(static_cast<long>(out.count), std::memory_order_relaxed) +
                       static_cast<long>(out.count);
            long p = peak.load(std::memory_order_relaxed);
            while (now > p && !peak.compare_exchange_weak(p, now, std::memory_order_relaxed)) {
            }
        }
        return out;
    }

    // Bulk-return: O(1) regardless of how many nodes are in `chain`.
    void release(Chain& chain) {
        if (chain.empty()) return;
        inUse.fetch_sub(static_cast<long>(chain.count), std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(m);
        freeChain.append(chain);
    }

    // Instrumentation — cheap (one atomic op per bulk acquire/release, not
    // per node), safe to leave enabled in production for memory monitoring.
    long inUseCount() const { return inUse.load(std::memory_order_relaxed); }
    long peakInUse() const { return peak.load(std::memory_order_relaxed); }
    size_t capacity() const { return arena.size(); }

    // Diagnostic-only helper (e.g. for tests building per-node "in use"
    // marker arrays) — maps a node back to its slot index in the arena.
    size_t indexOf(const JobNode* n) const { return static_cast<size_t>(n - arena.data()); }

private:
    std::vector<JobNode> arena;
    Chain freeChain;
    std::mutex m;
    std::atomic<long> inUse{0};
    std::atomic<long> peak{0};
};

// ---------------------------------------------------------------------------
// ThreadLocalPool: per-thread cache in front of the shared NodePool, so the
// common case (grab one node to build a candidate move) never touches the
// shared mutex. Refills/returns in bulk (whole chains) from/to the shared
// pool only when the local cache runs dry or is being torn down.
// ---------------------------------------------------------------------------
class ThreadLocalPool {
public:
    ThreadLocalPool(NodePool& shared, size_t refillSize) : shared(shared), refillSize(refillSize) {}

    JobNode* alloc() {
        if (local.empty()) local = shared.acquire(refillSize);
        JobNode* n = local.head;
        local.head = n->next;
        if (!local.head) local.tail = nullptr;
        --local.count;
        return n;
    }

    // Return a whole (possibly large) dead subtree's chain in O(1).
    void releaseChain(Chain chain) { shared.release(chain); }

    ~ThreadLocalPool() { shared.release(local); }

private:
    NodePool& shared;
    Chain local;
    size_t refillSize;
};
