#pragma once

#include "Chain.hpp"
#include "Inbox.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

// Replace with the real CUDA entry point. Input and output share the same
// contiguous storage; only the dedicated CUDA-stage thread calls this hook.
void launchCudaBatch(JobState* states, size_t n);

constexpr size_t MAX_BATCH = 1000;
constexpr size_t INBOX_CAPACITY = 64;
constexpr size_t DISPATCH_PIPELINE_SLOTS = 3;
constexpr auto DISPATCH_FLUSH_INTERVAL = std::chrono::milliseconds(2);

template <typename T, size_t Capacity> class BoundedPipelineQueue {
public:
    bool push(T value) {
        std::unique_lock<std::mutex> lock(mutex_);
        notFull_.wait(lock, [&] { return queue_.size() < Capacity || closed_; });
        if (closed_) return false;
        queue_.push_back(std::move(value));
        notEmpty_.notify_one();
        return true;
    }

    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait(lock, [&] { return !queue_.empty() || closed_; });
        if (queue_.empty()) return false;
        value = std::move(queue_.front());
        queue_.pop_front();
        notFull_.notify_one();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

private:
    std::deque<T> queue_;
    bool closed_ = false;
    std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
};

struct DispatcherStatsSnapshot {
    uint64_t batchesPrepared = 0;
    uint64_t jobsPrepared = 0;
    uint64_t fullBatches = 0;
    uint64_t deadlineFlushes = 0;
};

// Three-stage bounded dispatcher pipeline:
//
//  1. preprocess: fair inbox draining + JobState flattening. A partial batch
//     is flushed no later than 2ms after its first job is pulled; a full batch
//     is flushed immediately.
//  2. CUDA: the only thread that invokes launchCudaBatch().
//  3. postprocess: copy results back into JobNodes, split by owner, and return
//     chains through the existing per-producer result inboxes.
//
// The two inter-stage queues each hold at most three owning batch pointers.
// stop() drains all requests submitted before it was called, then closes each
// stage in order. Call stop only after all producer threads have joined.
class Dispatcher {
public:
    explicit Dispatcher(size_t numProducers)
        : inboxes_(numProducers), resultsInboxes_(numProducers), pending_(numProducers) {
        resultSignals_.reserve(numProducers);
        abandoned_.reserve(numProducers);
        for (size_t i = 0; i < numProducers; ++i)
            resultSignals_.push_back(std::make_unique<ResultSignal>());
        for (size_t i = 0; i < numProducers; ++i)
            abandoned_.push_back(std::make_unique<std::atomic<bool>>(false));
    }

    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;
    ~Dispatcher() { stop(); }

    void start() {
        bool expected = false;
        if (!started_.compare_exchange_strong(expected, true))
            throw std::logic_error("dispatcher already started");
        preprocessThread_ = std::thread([this] { preprocessLoop(); });
        cudaThread_ = std::thread([this] { cudaLoop(); });
        postprocessThread_ = std::thread([this] { postprocessLoop(); });
    }

    void stop() {
        if (!started_.load(std::memory_order_acquire)) return;
        bool expected = false;
        if (!stopRequested_.compare_exchange_strong(expected, true)) return;
        requestReady_.notify_all();
        if (preprocessThread_.joinable()) preprocessThread_.join();
        if (cudaThread_.joinable()) cudaThread_.join();
        if (postprocessThread_.joinable()) postprocessThread_.join();
    }

    void submit(size_t producerId, Chain chunk) {
        if (!started_.load(std::memory_order_acquire) || stopRequested_.load())
            throw std::logic_error("submit requires a running dispatcher");
        while (!inboxes_[producerId].push(chunk)) std::this_thread::yield();
        requestEpoch_.fetch_add(1, std::memory_order_release);
        requestReady_.notify_one();
    }

    void collect(size_t producerId, Chain& out) {
        ResultSignal& signal = *resultSignals_[producerId];
        uint64_t seen = signal.epoch.load(std::memory_order_acquire);
        while (!resultsInboxes_[producerId].pop(out)) {
            std::unique_lock<std::mutex> lock(signal.mutex);
            signal.ready.wait(lock, [&] {
                return signal.epoch.load(std::memory_order_acquire) != seen;
            });
            seen = signal.epoch.load(std::memory_order_acquire);
        }
    }

    bool collectCancellable(size_t producerId, Chain& out,
                            const std::atomic<bool>& cancelled) {
        ResultSignal& signal = *resultSignals_[producerId];
        uint64_t seen = signal.epoch.load(std::memory_order_acquire);
        while (!resultsInboxes_[producerId].pop(out)) {
            if (cancelled.load(std::memory_order_relaxed)) {
                abandonProducer(producerId);
                return false;
            }
            std::unique_lock<std::mutex> lock(signal.mutex);
            signal.ready.wait_for(lock, DISPATCH_FLUSH_INTERVAL, [&] {
                return signal.epoch.load(std::memory_order_acquire) != seen ||
                       cancelled.load(std::memory_order_relaxed);
            });
            seen = signal.epoch.load(std::memory_order_acquire);
        }
        return true;
    }

    bool tryCollect(size_t producerId, Chain& out) {
        return resultsInboxes_[producerId].pop(out);
    }

    // Global early termination only: this producer will never inspect another
    // result. Nodes remain arena-resident for process teardown/reconstruction,
    // but postprocessing must not block trying to fill its abandoned inbox.
    void abandonProducer(size_t producerId) {
        abandoned_[producerId]->store(true, std::memory_order_release);
        resultSignals_[producerId]->epoch.fetch_add(1, std::memory_order_release);
        resultSignals_[producerId]->ready.notify_all();
    }

    DispatcherStatsSnapshot stats() const {
        return {batchesPrepared_.load(std::memory_order_relaxed),
                jobsPrepared_.load(std::memory_order_relaxed),
                fullBatches_.load(std::memory_order_relaxed),
                deadlineFlushes_.load(std::memory_order_relaxed)};
    }

private:
    struct PreparedBatch {
        Chain nodes;
        std::vector<JobState> states;
    };

    struct ResultSignal {
        std::atomic<uint64_t> epoch{0};
        std::mutex mutex;
        std::condition_variable ready;
    };

    bool gatherAvailable(Chain& batch) {
        bool progress = false;
        for (size_t id = 0; id < inboxes_.size() && batch.count < MAX_BATCH; ++id) {
            if (pending_[id].empty()) inboxes_[id].pop(pending_[id]);
            if (pending_[id].empty()) continue;
            Chain selected = pending_[id].takeFirst(MAX_BATCH - batch.count);
            batch.append(selected);
            progress = true;
        }
        return progress;
    }

    bool anyRequestsReady() {
        for (size_t id = 0; id < inboxes_.size(); ++id) {
            if (!pending_[id].empty()) return true;
            Chain chunk;
            if (inboxes_[id].pop(chunk)) {
                pending_[id].append(chunk);
                return true;
            }
        }
        return false;
    }

    void prepareAndQueue(Chain& nodes, bool deadlineFlush) {
        auto batch = std::make_unique<PreparedBatch>();
        batch->nodes = nodes.takeAll();
        batch->states.reserve(batch->nodes.count);
        for (JobNode* node = batch->nodes.head; node; node = node->next)
            batch->states.push_back(node->state);

        batchesPrepared_.fetch_add(1, std::memory_order_relaxed);
        jobsPrepared_.fetch_add(batch->states.size(), std::memory_order_relaxed);
        if (batch->states.size() == MAX_BATCH)
            fullBatches_.fetch_add(1, std::memory_order_relaxed);
        if (deadlineFlush) deadlineFlushes_.fetch_add(1, std::memory_order_relaxed);
        prepared_.push(std::move(batch));
    }

    void preprocessLoop() {
        Chain assembling;
        std::chrono::steady_clock::time_point deadline;
        uint64_t seenEpoch = requestEpoch_.load(std::memory_order_acquire);

        for (;;) {
            bool progress = gatherAvailable(assembling);
            if (progress && assembling.count && deadline.time_since_epoch().count() == 0)
                deadline = std::chrono::steady_clock::now() + DISPATCH_FLUSH_INTERVAL;

            if (assembling.count == MAX_BATCH) {
                prepareAndQueue(assembling, false);
                deadline = {};
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            if (!assembling.empty() && now >= deadline) {
                prepareAndQueue(assembling, true);
                deadline = {};
                continue;
            }

            if (stopRequested_.load(std::memory_order_acquire)) {
                if (anyRequestsReady()) continue;
                if (!assembling.empty()) prepareAndQueue(assembling, false);
                break;
            }

            std::unique_lock<std::mutex> lock(requestMutex_);
            if (assembling.empty()) {
                requestReady_.wait(lock, [&] {
                    return stopRequested_.load(std::memory_order_acquire) ||
                           requestEpoch_.load(std::memory_order_acquire) != seenEpoch;
                });
            } else {
                requestReady_.wait_until(lock, deadline, [&] {
                    return stopRequested_.load(std::memory_order_acquire) ||
                           requestEpoch_.load(std::memory_order_acquire) != seenEpoch;
                });
            }
            seenEpoch = requestEpoch_.load(std::memory_order_acquire);
        }
        prepared_.close();
    }

    void cudaLoop() {
        std::unique_ptr<PreparedBatch> batch;
        while (prepared_.pop(batch)) {
            launchCudaBatch(batch->states.data(), batch->states.size());
            completed_.push(std::move(batch));
        }
        completed_.close();
    }

    void postprocessLoop() {
        std::unique_ptr<PreparedBatch> batch;
        while (completed_.pop(batch)) {
            size_t index = 0;
            for (JobNode* node = batch->nodes.head; node; node = node->next, ++index)
                node->state = batch->states[index];
            distributeResults(batch->nodes);
        }
    }

    void distributeResults(Chain& batch) {
        std::vector<Chain> perOwner(inboxes_.size());
        JobNode* node = batch.head;
        while (node) {
            JobNode* next = node->next;
            perOwner[node->ownerId].pushBack(node);
            node = next;
        }
        batch = {};
        for (size_t id = 0; id < perOwner.size(); ++id) {
            if (perOwner[id].empty()) continue;
            if (abandoned_[id]->load(std::memory_order_acquire)) continue;
            while (!resultsInboxes_[id].push(perOwner[id])) {
                if (abandoned_[id]->load(std::memory_order_acquire)) break;
                std::this_thread::yield();
            }
            if (abandoned_[id]->load(std::memory_order_acquire)) continue;
            resultSignals_[id]->epoch.fetch_add(1, std::memory_order_release);
            resultSignals_[id]->ready.notify_one();
        }
    }

    std::vector<ChainInbox<INBOX_CAPACITY>> inboxes_;
    std::vector<ChainInbox<INBOX_CAPACITY>> resultsInboxes_;
    std::vector<Chain> pending_;
    std::vector<std::unique_ptr<ResultSignal>> resultSignals_;
    std::vector<std::unique_ptr<std::atomic<bool>>> abandoned_;
    BoundedPipelineQueue<std::unique_ptr<PreparedBatch>, DISPATCH_PIPELINE_SLOTS> prepared_;
    BoundedPipelineQueue<std::unique_ptr<PreparedBatch>, DISPATCH_PIPELINE_SLOTS> completed_;

    std::atomic<bool> started_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<uint64_t> requestEpoch_{0};
    std::mutex requestMutex_;
    std::condition_variable requestReady_;
    std::thread preprocessThread_;
    std::thread cudaThread_;
    std::thread postprocessThread_;

    std::atomic<uint64_t> batchesPrepared_{0};
    std::atomic<uint64_t> jobsPrepared_{0};
    std::atomic<uint64_t> fullBatches_{0};
    std::atomic<uint64_t> deadlineFlushes_{0};
};
