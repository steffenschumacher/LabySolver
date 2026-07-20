#include "../LevelCatalog.hpp"
#include "../ReferenceSolver.hpp"
#include "test_util.hpp"

#include <cstdio>

using namespace laby;

int main() {
    unsigned solvedCount = 0;
    uint64_t examined = 0;
    for (size_t number = 1; number <= LEVEL_CATALOG.size(); ++number) {
        const LevelDefinition level = loadLevel(number);
        if (level.maxPushes > 4) continue;
        const SolveResult result = solveMinimum(level);
        std::printf("  level %zu solved=%d depth=%zu states=%llu\n", number, result.solved,
                    result.moves.size(), static_cast<unsigned long long>(result.statesExamined));
        CHECK(result.minimumProven);
        CHECK(result.solved);
        CHECK(result.moves.size() <= level.maxPushes);
        CHECK(replaySolution(level, result.moves));
        solvedCount += result.solved;
        examined += result.statesExamined;
    }
    CHECK(solvedCount == 17);
    std::printf("Reference solver: %u depth-2/3/4 levels, %llu states examined\n", solvedCount,
                static_cast<unsigned long long>(examined));
    REPORT();
    return 0;
}
