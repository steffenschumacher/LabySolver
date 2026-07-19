#pragma once
#include "JobState.hpp"
#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// JobNode: one candidate move/job. Lives forever in a preallocated arena
// (see NodePool.hpp) — never individually new'd or delete'd, only ever
// moved between chains via pointer splicing.
// ---------------------------------------------------------------------------
struct JobNode {
    JobState state;   // input fields going in; CUDA writes results in place
    JobNode* parent;  // for path reconstruction back to the seed
    JobNode* next;    // intrusive singly-linked pointer
    uint32_t ownerId; // which producer (0 = master, 1..N = workers)
    uint32_t level;   // depth this node represents (1..7)
    // Used by the bounded asynchronous Worker. A node remains resident until
    // its expansion has been submitted and every child subtree has completed
    // negatively. Master does not depend on these fields.
    uint32_t outstandingChildren;
    bool expansionComplete;
};

// ---------------------------------------------------------------------------
// Chain: a lightweight, cheap-to-copy descriptor over a run of JobNodes.
// It does not "own" memory in the allocation sense — it just describes a
// splice range. All operations below are O(1) regardless of chain length,
// except pushBack (single node) which is trivially O(1) too.
// ---------------------------------------------------------------------------
struct Chain {
    JobNode* head = nullptr;
    JobNode* tail = nullptr;
    size_t count = 0;

    bool empty() const { return head == nullptr; }

    // O(1): attach a single node to the tail.
    void pushBack(JobNode* n) {
        n->next = nullptr;
        if (!head) {
            head = tail = n;
        } else {
            tail->next = n;
            tail = n;
        }
        ++count;
    }

    // O(1): splice another whole chain onto our tail; drains `other`.
    void append(Chain& other) {
        if (other.empty()) return;
        if (empty()) {
            *this = other;
        } else {
            tail->next = other.head;
            tail = other.tail;
            count += other.count;
        }
        other = Chain{};
    }

    // O(1): hand out the whole chain, leaving this one empty.
    // Use this instead of copying `*this` around when you mean to transfer
    // ownership/responsibility (e.g. submitting to the dispatcher).
    Chain takeAll() {
        Chain result = *this;
        *this = Chain{};
        return result;
    }

    // Detach and return up to `limit` nodes from the front. This is O(limit)
    // because the list is singly linked. Dispatcher uses it only at a hard
    // batch boundary, where walking the selected nodes is unavoidable anyway.
    Chain takeFirst(size_t limit) {
        if (limit == 0 || empty()) return {};
        if (limit >= count) return takeAll();

        Chain result;
        result.head = head;
        result.tail = head;
        for (size_t i = 1; i < limit; ++i) result.tail = result.tail->next;
        head = result.tail->next;
        result.tail->next = nullptr;
        result.count = limit;
        count -= limit;
        return result;
    }

    static Chain single(JobNode* n) {
        Chain c;
        c.pushBack(n);
        return c;
    }
};
