#include "../SearchInstrumentation.hpp"
#include "test_util.hpp"

#include <array>
#include <cmath>
#include <limits>

int main() {
    SearchInstrumentation instrumentation;

    // Synthetic regular tree: branching 3 through the master depths.
    instrumentation.recordExpansion(0, 3, true);
    instrumentation.recordMasterSurvivors(1, 3);
    for (int i = 0; i < 3; ++i) {
        instrumentation.recordExpansion(1, 3, true);
        instrumentation.recordMasterSurvivors(2, 3);
    }
    for (int i = 0; i < 9; ++i) {
        instrumentation.recordExpansion(2, 3, true);
        instrumentation.recordMasterSurvivors(3, 3);
    }
    for (int i = 0; i < 27; ++i) {
        instrumentation.recordExpansion(3, 3, true);
        instrumentation.recordMasterSurvivors(4, 3);
    }
    for (int i = 0; i < 81; ++i) instrumentation.recordSeedEmitted();

    std::array<uint64_t, INSTRUMENTED_DEPTHS> seedJobs{};
    seedJobs[5] = 3;
    seedJobs[6] = 9;
    seedJobs[7] = 27;
    for (int i = 0; i < 10; ++i) instrumentation.recordCompletedSeed(seedJobs);

    SearchEstimate running = instrumentation.snapshot();
    CHECK(running.completedSeeds == 10);
    CHECK(running.emittedSeeds == 81);
    CHECK(std::fabs(running.estimatedJobs[4] - 81.0) < 0.001);
    CHECK(std::fabs(running.estimatedJobs[7] - 2187.0) < 0.001);
    CHECK(std::fabs(running.estimatedTotalJobs - 3279.0) < 0.001);
    CHECK(!running.masterFinished);

    instrumentation.markMasterFinished();
    SearchEstimate finished = instrumentation.snapshot();
    CHECK(finished.masterFinished);
    CHECK(finished.observedJobs[1] == 3);
    CHECK(finished.observedJobs[4] == 81);
    CHECK(std::fabs(finished.averageBranching[4] - 3.0) < 0.001);
    CHECK(std::fabs(finished.estimatedTotalJobs - 3279.0) < 0.001);

    // Wire codec used by remote metrics is tested in test_remote_transport;
    // malformed/empty estimators remain safe and report zero.
    SearchInstrumentation empty;
    CHECK(empty.snapshot().estimatedTotalJobs == 0.0);

    // Exact display counters saturate rather than wrapping, while the
    // long-double running statistics continue beyond uint64_t range.
    SearchInstrumentation huge;
    huge.recordExpansion(0, std::numeric_limits<uint64_t>::max(), true);
    huge.recordExpansion(0, 42, true);
    SearchEstimate hugeSnapshot = huge.snapshot();
    CHECK(hugeSnapshot.observedJobs[1] == std::numeric_limits<uint64_t>::max());
    CHECK(hugeSnapshot.observedJobsOverflowed[1]);
    CHECK(hugeSnapshot.observedJobsApprox[1] >
          static_cast<long double>(std::numeric_limits<uint64_t>::max()));
    CHECK(std::fabs(hugeSnapshot.averageBranching[1] -
                    (static_cast<long double>(std::numeric_limits<uint64_t>::max()) + 42.0L) /
                        2.0L) < 2.0L);

    REPORT();
}
