#pragma once

#include "RemoteTransport.hpp"
#include "Worker.hpp"

#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace remote {

struct WorkerHostResult {
    uint64_t seedsReceived = 0;
    bool solutionFound = false;
};

// Platform-neutral remote-host orchestration. The Windows executable supplies
// the same board hooks and launchCudaBatch implementation as Linux; this class
// owns only transport, queues, worker threads, and lifecycle/error propagation.
class RemoteWorkerHost {
public:
    RemoteWorkerHost(std::shared_ptr<FramedSocket> coordinator, size_t workerThreads,
                     Dispatcher& dispatcher, NodePool& pool,
                     WorkerSchedulerConfig workerConfig = {})
        : coordinator_(std::move(coordinator)), workerThreads_(workerThreads),
          dispatcher_(dispatcher), pool_(pool), workerConfig_(workerConfig) {
        if (!coordinator_ || workerThreads_ == 0)
            throw TransportError("remote worker host requires workers and a coordinator");
    }

    WorkerHostResult run() {
        SeedQueue<32> seeds;
        SearchGlobals globals;
        RemoteMetricsSink metrics(coordinator_);
        SeedReceiver receiver(coordinator_);
        std::exception_ptr receiverFailure;
        uint64_t received = 0;

        dispatcher_.start();
        std::thread receiverThread([&] {
            try {
                received = receiver.run(seeds);
            } catch (...) {
                receiverFailure = std::current_exception();
            }
        });

        std::vector<Worker> workers;
        workers.reserve(workerThreads_);
        for (size_t i = 0; i < workerThreads_; ++i)
            workers.emplace_back(i + 1, dispatcher_, pool_, seeds, globals, &metrics, workerConfig_);
        std::vector<std::thread> threads;
        threads.reserve(workerThreads_);
        for (Worker& worker : workers) threads.emplace_back([&worker] { worker.run(); });

        receiverThread.join();
        for (std::thread& thread : threads) thread.join();
        metrics.finished();
        dispatcher_.stop();
        if (receiverFailure) std::rethrow_exception(receiverFailure);
        return WorkerHostResult{received, globals.solutionFound.load(std::memory_order_acquire)};
    }

private:
    std::shared_ptr<FramedSocket> coordinator_;
    size_t workerThreads_;
    Dispatcher& dispatcher_;
    NodePool& pool_;
    WorkerSchedulerConfig workerConfig_;
};

} // namespace remote
