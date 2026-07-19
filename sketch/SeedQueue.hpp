#pragma once
#include "JobState.hpp"
#include <array>
#include <mutex>
#include <condition_variable>
#include <cstdint>

// A depth-N prefix handed from the master to a worker (N = however many
// levels the master owns, e.g. moves 1..4 -- see Master.hpp's
// MASTER_DEPTH): the resulting board state, how many moves produced it, and
// those moves themselves (needed later to reconstruct the full 7-move
// solution). `moves`/`depth` are sized generously (fixed at the overall
// move budget) rather than tied to MASTER_DEPTH exactly, so tuning the
// master/worker split later doesn't require touching this struct.
constexpr size_t SEED_MAX_MOVES = 8; // must be >= the real move budget (7)
struct Seed {
    JobState state;
    struct Move {
        uint8_t insertPoint;
        uint8_t orientation;
    };
    uint8_t depth; // how many of the entries below are valid
    Move moves[SEED_MAX_MOVES];
};

// ---------------------------------------------------------------------------
// SeedQueue: bounded, blocking queue between master (single producer) and
// workers (multiple consumers). Push blocks when full (throttles master's
// generation rate to match consumption); pop blocks when empty unless the
// master has signalled finished(), in which case it returns false so the
// worker can retire.
//
// This one push/pop happens roughly once per exhausted worker-subtree (i.e.
// rarely, relative to CUDA batch cadence), so a plain mutex+condvar is fine
// — no need for lock-free machinery here.
// ---------------------------------------------------------------------------
template <size_t Capacity> class SeedQueue {
public:
    // Returns false if the push was discarded because the queue was
    // aborted (solution already found elsewhere) or finished. Callers that
    // don't care can ignore the return value.
    bool push(const Seed& s) {
        std::unique_lock<std::mutex> lk(m);
        notFull.wait(lk, [&] { return count < Capacity || done || aborted; });
        if (done || aborted) return false;
        buf[tail] = s;
        tail = (tail + 1) % Capacity;
        ++count;
        notEmpty.notify_one();
        return true;
    }

    // Returns false once the queue is empty AND (finished() was called OR
    // abort() was called) -- i.e. there will never be anything more to pop.
    bool pop(Seed& out) {
        std::unique_lock<std::mutex> lk(m);
        notEmpty.wait(lk, [&] { return count > 0 || done || aborted; });
        if (count == 0) return false;
        out = buf[head];
        head = (head + 1) % Capacity;
        --count;
        notFull.notify_one();
        return true;
    }

    // Master calls this once all depth-3 seeds have been enqueued.
    void finished() {
        std::lock_guard<std::mutex> lk(m);
        done = true;
        notEmpty.notify_all();
        notFull.notify_all();
    }

    // Called by whichever thread (master or worker) first discovers the
    // solution, so that anyone currently blocked in push()/pop() wakes
    // immediately instead of waiting for an unrelated queue event that may
    // never come. This is what prevents the deadlock where: a worker finds
    // the solution and exits its run() loop (which no longer calls pop()
    // once solutionFound is true) while the master is simultaneously
    // blocked inside push() on a full queue -- with nothing left to drain
    // it, notFull would otherwise never be signalled again.
    void abort() {
        std::lock_guard<std::mutex> lk(m);
        aborted = true;
        notEmpty.notify_all();
        notFull.notify_all();
    }

private:
    std::array<Seed, Capacity> buf{};
    size_t head = 0, tail = 0, count = 0;
    bool done = false;
    bool aborted = false;
    std::mutex m;
    std::condition_variable notFull, notEmpty;
};
