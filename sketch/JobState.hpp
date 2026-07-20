#pragma once
#include "BoardState.hpp"
#include <cstdint>
#include <type_traits>

// ---------------------------------------------------------------------------
// Dispatcher record: a 23-byte canonical board plus five bytes of move/result
// metadata. Routing ownership and tree bookkeeping live in JobNode, not here.
// `boardBytes` is a temporary source-compatible view used by the synthetic
// scheduler tests; production game code uses `board`.
// ---------------------------------------------------------------------------
struct JobState {
    union {
        uint8_t boardBytes[sizeof(laby::CompactBoardState)];
        laby::CompactBoardState board;
    };
    uint8_t insertPoint;    // which of the 10 entries this move used
    uint8_t orientation;    // 0..3
    uint8_t reachableMask;  // CUDA kernel output: which bugs are reachable
    uint8_t bugsEatenMask;  // CUDA kernel output: which bugs got eaten
    uint8_t offBoard;       // CUDA kernel output: 1 if a bug got pushed off
};

static_assert(std::is_trivially_copyable_v<JobState>, "JobState must stay POD");
static_assert(sizeof(JobState) == 28, "dispatcher state size regression");
