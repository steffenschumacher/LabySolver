#include "../BoardState.hpp"
#include "../JobState.hpp"
#include "test_util.hpp"

#include <cstdio>

using namespace laby;

int main() {
    CompactBoardState state{};

    for (uint8_t i = 0; i <= SPARE_POSITION; ++i) state.setTile(i, i & 0x0f);
    for (uint8_t i = 0; i <= SPARE_POSITION; ++i) CHECK(state.tile(i) == (i & 0x0f));

    for (uint8_t slot = 0; slot < POSITION_COUNT; ++slot) {
        for (uint8_t value = 0; value <= UNUSED_GOAL_POSITION; ++value) {
            state.setPosition(slot, value);
            CHECK(state.position(slot) == value);
        }
    }

    state.setLadybug(35);
    state.setGoal(0, 1);
    state.setGoal(1, 9);
    state.setGoal(2, UNUSED_GOAL_POSITION);
    state.setGoal(3, UNUSED_GOAL_POSITION);
    CHECK(state.goalCount() == 2);
    state.setNextGoal(1);
    state.setDepth(7);
    CHECK(state.nextGoal() == 1);
    CHECK(state.depth() == 7);
    CHECK(!state.won());
    state.setNextGoal(2);
    CHECK(state.won());

    CHECK(rotateClockwise(North) == East);
    CHECK(rotateClockwise(East) == South);
    CHECK(rotateClockwise(South) == West);
    CHECK(rotateClockwise(West) == North);
    CHECK(rotateClockwise(0x0f) == 0x0f);
    CHECK(sizeof(CompactBoardState) == 23);
    CHECK(sizeof(JobState) == 28);

    std::printf("CompactBoardState: %zu bytes\n", sizeof(CompactBoardState));
    REPORT();
    return 0;
}
