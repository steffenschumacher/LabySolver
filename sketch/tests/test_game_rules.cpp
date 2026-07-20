#include "../GameRules.hpp"
#include "../ReferenceSolver.hpp"
#include "test_util.hpp"

using namespace laby;

static CompactBoardState emptyState() {
    CompactBoardState s{};
    for (uint8_t i = 0; i <= SPARE_POSITION; ++i) s.setTile(i, 0);
    s.setLadybug(0);
    for (uint8_t i = 0; i < 4; ++i) s.setGoal(i, UNUSED_GOAL_POSITION);
    s.setGoal(0, 1);
    return s;
}

int main() {
    auto s = emptyState();
    s.setTile(0, East);
    s.setTile(1, West);
    CHECK(reachableCells(s, 0) == 0x3);
    s.setTile(1, East);
    CHECK(reachableCells(s, 0) == 0x1);

    s = emptyState();
    s.setTile(0, East);
    s.setTile(1, East | West);
    s.setTile(2, West);
    s.setGoal(0, 1);
    s.setGoal(1, 2);
    s.setGoal(2, UNUSED_GOAL_POSITION);
    normalizeGoals(s);
    CHECK(s.nextGoal() == 2);
    s.setNextGoal(0);
    s.setGoal(0, 10);
    s.setGoal(1, 1);
    normalizeGoals(s);
    CHECK(s.nextGoal() == 0);

    s = emptyState();
    for (uint8_t x = 0; x < BOARD_WIDTH; ++x) s.setTile(cellIndex(x, 1), x + 1);
    s.setTile(SPARE_POSITION, 9);
    s.setLadybug(cellIndex(4, 1));
    s.setGoal(0, SPARE_POSITION);
    CompactBoardState child;
    CHECK(applyMove(child, s, Move{0, 0, cellIndex(4, 1)}, true));
    CHECK(child.tile(cellIndex(0, 1)) == 9);
    CHECK(child.tile(cellIndex(1, 1)) == 1);
    CHECK(child.tile(SPARE_POSITION) == 5);
    CHECK(child.ladybug() == SPARE_POSITION);
    CHECK(child.goal(0) == cellIndex(0, 1));
    CHECK(!applyMove(child, s, Move{0, 0, cellIndex(4, 1)}, false));

    s.setLadybug(SPARE_POSITION);
    CHECK(applyCandidate(child, s, Move{0, 0, SPARE_POSITION}, true));
    CHECK(child.ladybug() == cellIndex(0, 1));

    CHECK(distinctRotations(0).size() == 1);
    CHECK(distinctRotations(0x0f).size() == 1);
    CHECK(distinctRotations(North | South).size() == 2);
    CHECK(distinctRotations(North).size() == 4);
    CHECK(distinctRotations(North | East | West).size() == 4);

    LevelDefinition level;
    level.initial = emptyState();
    level.initial.setLadybug(cellIndex(0, 1));
    level.initial.setGoal(0, SPARE_POSITION);
    level.initial.setTile(cellIndex(0, 1), West);
    level.initial.setTile(SPARE_POSITION, East);
    level.maxPushes = 1;
    level.mayPushPlayerOut = true;
    const SolveResult solved = solveMinimum(level);
    CHECK(solved.solved);
    CHECK(solved.minimumProven);
    CHECK(solved.moves.size() == 1);
    CHECK(replaySolution(level, solved.moves));

    REPORT();
    return 0;
}
