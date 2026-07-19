#pragma once
#include <cstdio>

// Minimal, dependency-free check/report macros — no test framework needed
// for these small standalone binaries.
inline int& checkCount() {
    static int c = 0;
    return c;
}
inline int& failCount() {
    static int c = 0;
    return c;
}

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++checkCount();                                                                            \
        if (!(cond)) {                                                                             \
            ++failCount();                                                                         \
            std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__);         \
        }                                                                                          \
    } while (0)

#define REPORT()                                                                                   \
    do {                                                                                           \
        std::printf("%d/%d checks passed\n", checkCount() - failCount(), checkCount());            \
        if (failCount()) return 1;                                                                 \
    } while (0)
