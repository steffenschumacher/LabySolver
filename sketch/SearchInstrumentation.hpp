#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

constexpr size_t INSTRUMENTED_DEPTHS = 8; // indices 0..7

struct SearchEstimate {
    std::array<uint64_t, INSTRUMENTED_DEPTHS> observedJobs{};
    std::array<bool, INSTRUMENTED_DEPTHS> observedJobsOverflowed{};
    std::array<uint64_t, INSTRUMENTED_DEPTHS> expansions{};
    std::array<bool, INSTRUMENTED_DEPTHS> expansionsOverflowed{};
    std::array<long double, INSTRUMENTED_DEPTHS> observedJobsApprox{};
    std::array<long double, INSTRUMENTED_DEPTHS> averageBranching{};
    std::array<long double, INSTRUMENTED_DEPTHS> estimatedJobs{};
    uint64_t completedSeeds = 0;
    bool completedSeedsOverflowed = false;
    uint64_t emittedSeeds = 0;
    bool emittedSeedsOverflowed = false;
    long double estimatedSeeds = 0;
    long double estimatedTotalJobs = 0;
    bool masterFinished = false;
};

class SearchInstrumentationSink {
public:
    virtual ~SearchInstrumentationSink() = default;
    virtual void recordExpansion(size_t parentDepth, uint64_t children, bool masterProducer) = 0;
    virtual void recordCompletedSeed(
        const std::array<uint64_t, INSTRUMENTED_DEPTHS>& jobsByDepth) = 0;
};

// Low-cost shared instrumentation. One locked update is made per expanded
// search node and one per completed seed; no per-job atomic operation is
// required. Depth-4 seed samples let the coordinator estimate the otherwise
// unknown work below the master/worker boundary.
class SearchInstrumentation : public SearchInstrumentationSink {
public:
    void recordExpansion(size_t parentDepth, uint64_t children, bool masterProducer) override {
        if (parentDepth + 1 >= INSTRUMENTED_DEPTHS) return;
        std::lock_guard<std::mutex> lock(mutex_);
        saturatingIncrement(expansions_[parentDepth], expansionsOverflowed_[parentDepth]);
        saturatingAdd(jobs_[parentDepth + 1], children, jobsOverflowed_[parentDepth + 1]);
        jobsApprox_[parentDepth + 1] += static_cast<long double>(children);
        updateMean(averageBranching_[parentDepth + 1], branchingSamples_[parentDepth + 1],
                   static_cast<long double>(children));
        if (masterProducer) {
            masterJobsApprox_[parentDepth + 1] += static_cast<long double>(children);
            updateMean(masterAverageBranching_[parentDepth + 1],
                       masterBranchingSamples_[parentDepth + 1],
                       static_cast<long double>(children));
        }
    }

    void recordCompletedSeed(
        const std::array<uint64_t, INSTRUMENTED_DEPTHS>& jobsByDepth) override {
        std::lock_guard<std::mutex> lock(mutex_);
        saturatingIncrement(completedSeeds_, completedSeedsOverflowed_);
        completedSeedsApprox_ += 1.0L;
        for (size_t depth = 0; depth < INSTRUMENTED_DEPTHS; ++depth) {
            // Every depth uses the same sample count, including zero-job
            // depths. Online means avoid an unbounded sum of per-seed jobs.
            long double delta = static_cast<long double>(jobsByDepth[depth]) - meanSeedJobs_[depth];
            meanSeedJobs_[depth] += delta / completedSeedsApprox_;
        }
    }

    void recordSeedEmitted() {
        std::lock_guard<std::mutex> lock(mutex_);
        saturatingIncrement(emittedSeeds_, emittedSeedsOverflowed_);
        emittedSeedsApprox_ += 1.0L;
    }

    void recordMasterSurvivors(size_t childDepth, uint64_t survivors) {
        if (childDepth >= INSTRUMENTED_DEPTHS) return;
        std::lock_guard<std::mutex> lock(mutex_);
        updateMean(masterAverageSurvivors_[childDepth], masterSurvivorSamples_[childDepth],
                   static_cast<long double>(survivors));
    }

    void markMasterFinished() {
        std::lock_guard<std::mutex> lock(mutex_);
        masterFinished_ = true;
    }

    SearchEstimate snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        SearchEstimate result;
        result.observedJobs = jobs_;
        result.observedJobsOverflowed = jobsOverflowed_;
        result.expansions = expansions_;
        result.expansionsOverflowed = expansionsOverflowed_;
        result.observedJobsApprox = jobsApprox_;
        result.completedSeeds = completedSeeds_;
        result.completedSeedsOverflowed = completedSeedsOverflowed_;
        result.emittedSeeds = emittedSeeds_;
        result.emittedSeedsOverflowed = emittedSeedsOverflowed_;
        result.masterFinished = masterFinished_;

        result.averageBranching = averageBranching_;

        // Master depths: use exact counts after completion. While it is still
        // running, extrapolate each level from completed parent expansions.
        long double estimatedParents = 1.0L;
        for (size_t depth = 1; depth <= 4; ++depth) {
            if (masterFinished_)
                result.estimatedJobs[depth] = masterJobsApprox_[depth];
            else if (masterBranchingSamples_[depth] > 0)
                result.estimatedJobs[depth] =
                    estimatedParents * masterAverageBranching_[depth];

            if (masterSurvivorSamples_[depth] > 0)
                estimatedParents *= masterAverageSurvivors_[depth];
            else
                estimatedParents = 0;
        }

        // Only surviving depth-4 candidates become seeds. Never scale worker
        // samples by all depth-4 jobs, since that includes candidates pruned
        // by the kernel. Once Master finishes the emitted count is exact;
        // while running, use the observed depth-4 survival ratio.
        if (masterFinished_) {
            result.estimatedSeeds = emittedSeedsApprox_;
        } else {
            result.estimatedSeeds = estimatedParents;
        }

        // A completed worker seed supplies the actual aggregate number of
        // depth-5/6/7 jobs beneath one depth-4 state. Scale that sample mean by
        // the estimated total number of depth-4 seeds.
        for (size_t depth = 5; depth < INSTRUMENTED_DEPTHS; ++depth) {
            if (completedSeedsApprox_ > 0) {
                result.estimatedJobs[depth] = result.estimatedSeeds * meanSeedJobs_[depth];
            } else if (result.averageBranching[depth] > 0) {
                result.estimatedJobs[depth] =
                    result.estimatedJobs[depth - 1] * result.averageBranching[depth];
            }
        }

        for (size_t depth = 1; depth < INSTRUMENTED_DEPTHS; ++depth)
            result.estimatedTotalJobs += result.estimatedJobs[depth];
        return result;
    }

private:
    static void saturatingAdd(uint64_t& value, uint64_t increment, bool& overflowed) {
        const uint64_t maximum = std::numeric_limits<uint64_t>::max();
        if (increment > maximum - value) {
            value = maximum;
            overflowed = true;
        } else {
            value += increment;
        }
    }

    static void saturatingIncrement(uint64_t& value, bool& overflowed) {
        saturatingAdd(value, 1, overflowed);
    }

    static void updateMean(long double& mean, long double& samples, long double observation) {
        samples += 1.0L;
        mean += (observation - mean) / samples;
    }

    mutable std::mutex mutex_;
    std::array<uint64_t, INSTRUMENTED_DEPTHS> jobs_{};
    std::array<bool, INSTRUMENTED_DEPTHS> jobsOverflowed_{};
    std::array<long double, INSTRUMENTED_DEPTHS> jobsApprox_{};
    std::array<uint64_t, INSTRUMENTED_DEPTHS> expansions_{};
    std::array<bool, INSTRUMENTED_DEPTHS> expansionsOverflowed_{};
    std::array<long double, INSTRUMENTED_DEPTHS> averageBranching_{};
    std::array<long double, INSTRUMENTED_DEPTHS> branchingSamples_{};
    std::array<long double, INSTRUMENTED_DEPTHS> masterJobsApprox_{};
    std::array<long double, INSTRUMENTED_DEPTHS> masterAverageBranching_{};
    std::array<long double, INSTRUMENTED_DEPTHS> masterBranchingSamples_{};
    std::array<long double, INSTRUMENTED_DEPTHS> masterAverageSurvivors_{};
    std::array<long double, INSTRUMENTED_DEPTHS> masterSurvivorSamples_{};
    std::array<long double, INSTRUMENTED_DEPTHS> meanSeedJobs_{};
    uint64_t completedSeeds_ = 0;
    bool completedSeedsOverflowed_ = false;
    long double completedSeedsApprox_ = 0;
    uint64_t emittedSeeds_ = 0;
    bool emittedSeedsOverflowed_ = false;
    long double emittedSeedsApprox_ = 0;
    bool masterFinished_ = false;
};
