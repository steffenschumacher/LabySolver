#pragma once
#include "Chain.hpp"
#include "Inbox.hpp"
#include <vector>
#include <cstdint>
#include <thread>

// Replace with your existing CUDA entry point. `n` states in; results
// (reachableMask / bugsEatenMask / offBoard / resulting board bytes) are
// written back into the same JobState slots in place, so no separate
// result buffer is needed on the host side.
void launchCudaBatch(JobState* states, size_t n);

constexpr size_t MAX_BATCH = 1000;
constexpr size_t INBOX_CAPACITY = 64; // # of ~200-node chunks queued per producer

// ---------------------------------------------------------------------------
// Dispatcher: the only thing that talks to the GPU. Producers (master +
// workers) each get a dedicated request inbox and a dedicated results
// inbox, so no producer ever contends with another for a lock. This thread
// (call runOnce() in a loop from one dedicated dispatcher thread) fairly
// round-robins across producers' ready chunks to build each batch, so a
// producer with a huge backlog (e.g. a worker deep in an 810k-leaf level)
// can never starve the others out — it contributes at most one ~200-node
// chunk per round, same as everyone else.
// ---------------------------------------------------------------------------
class Dispatcher {
public:
    explicit Dispatcher(size_t numProducers) : inboxes(numProducers), resultsInboxes(numProducers) {
        staging.reserve(MAX_BATCH);
    }

    // Producers call this from their own thread to submit a ready chunk.
    // Backs off briefly if the inbox is momentarily full.
    void submit(size_t producerId, Chain chunk) {
        while (!inboxes[producerId].push(chunk)) std::this_thread::yield();
    }

    // Producers call this (blocking) to collect one finished chunk back.
    void collect(size_t producerId, Chain& out) {
        while (!resultsInboxes[producerId].pop(out)) std::this_thread::yield();
    }

    // Non-blocking variant: returns false immediately if nothing is ready
    // yet. Producers should call this opportunistically *while still
    // submitting* a level's worth of work (not only after submitting
    // everything) — otherwise, once a producer's uncollected results
    // exceed the results inbox's fixed capacity, the dispatcher thread
    // itself blocks trying to push more results for that producer, which
    // stalls every other producer too (there's only one dispatcher
    // thread). Interleaving submit/collect keeps each producer's
    // outstanding-results count bounded by construction.
    bool tryCollect(size_t producerId, Chain& out) { return resultsInboxes[producerId].pop(out); }

    // Call in a tight loop from the dedicated dispatcher thread.
    // Returns false if nothing was ready this round (caller may sleep briefly).
    bool runOnce() {
        Chain batch;
        bool madeProgress = true;
        while (batch.count < MAX_BATCH && madeProgress) {
            madeProgress = false;
            for (auto& inbox : inboxes) {
                if (batch.count >= MAX_BATCH) break;
                Chain chunk;
                if (inbox.pop(chunk)) {
                    batch.append(chunk);
                    madeProgress = true;
                }
            }
        }
        if (batch.empty()) return false;

        flattenRunAndWriteBack(batch);
        distributeResults(batch);
        return true;
    }

private:
    // The one unavoidable copy: GPU needs one contiguous buffer, but our
    // job data lives scattered across the producers' preallocated arenas.
    // `staging` is reused every call — sized once, never reallocated.
    void flattenRunAndWriteBack(Chain& batch) {
        staging.clear();
        for (JobNode* n = batch.head; n; n = n->next) staging.push_back(n->state);

        launchCudaBatch(staging.data(), staging.size());

        size_t i = 0;
        for (JobNode* n = batch.head; n; n = n->next, ++i) n->state = staging[i];
    }

    // Split the combined batch back into per-owner chains (O(batch size)
    // pointer relinking, no payload copying) and hand each back to its
    // producer's results inbox.
    void distributeResults(Chain& batch) {
        std::vector<Chain> perOwner(inboxes.size());
        JobNode* n = batch.head;
        while (n) {
            JobNode* next = n->next;
            perOwner[n->ownerId].pushBack(n);
            n = next;
        }
        for (size_t id = 0; id < perOwner.size(); ++id) {
            if (perOwner[id].empty()) continue;
            while (!resultsInboxes[id].push(perOwner[id])) std::this_thread::yield();
        }
    }

    std::vector<ChainInbox<INBOX_CAPACITY>> inboxes;
    std::vector<ChainInbox<INBOX_CAPACITY>> resultsInboxes;
    std::vector<JobState> staging;
};
