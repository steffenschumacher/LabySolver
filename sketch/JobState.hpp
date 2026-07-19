#pragma once
#include <cstdint>
#include <type_traits>

// ---------------------------------------------------------------------------
// Placeholder for your real ~80B board/tile/lady/bug encoding + CUDA result
// fields. Replace the body, but keep it POD / trivially copyable: the whole
// pipeline below assumes JobState can be memcpy'd freely and needs no
// constructor/destructor/virtual logic, because it lives in a preallocated
// arena and is never individually new'd or deleted.
// ---------------------------------------------------------------------------
struct JobState {
    uint8_t boardBytes[64]; // board + loose tile + bug/lady positions, etc.
    uint8_t insertPoint;    // which of the 10 entries this move used
    uint8_t orientation;    // 0..3
    uint8_t reachableMask;  // CUDA kernel output: which bugs are reachable
    uint8_t bugsEatenMask;  // CUDA kernel output: which bugs got eaten
    uint8_t offBoard;       // CUDA kernel output: 1 if a bug got pushed off
};

static_assert(std::is_trivially_copyable_v<JobState>, "JobState must stay POD");
