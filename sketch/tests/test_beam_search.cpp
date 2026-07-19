// Verifies the heuristic-first beam search (BeamSearch.hpp) against a
// synthetic board designed to contain a misleading local optimum: the
// heuristic rewards matching a "decoy" path that never actually wins,
// diverging from the real solution partway through. A pure greedy beam
// (no diversity, no restarts) should fail to find the real solution;
// enabling the diversity slice + a few restarts should let it escape the
// decoy and find the planted winning path -- demonstrating the mechanism
// the design relies on to avoid getting stuck behind the heuristic's blind
// spots.
#include "../BeamSearch.hpp"
#include "test_util.hpp"

#include <cstring>
#include <cstdio>

constexpr int BRANCHING = 6;
constexpr int TOTAL_DEPTH = 7;

// The heuristic greedily chases this path (it's "close" by the score
// function below) but it is NOT a winning path. TARGET_PATH shares the
// decoy's first 5 moves (so greedy descent is "correct" early on, just
// like a real board where the heuristic is generally trustworthy) and only
// diverges on the last two moves -- a trap a pure-greedy beam walks
// straight past, but that the diversity slice has a real chance to escape.
static const uint8_t DECOY_PATH[TOTAL_DEPTH] = {2, 4, 1, 5, 0, 3, 2};
static const uint8_t TARGET_PATH[TOTAL_DEPTH] = {2, 4, 1, 5, 0, 1, 5};

std::vector<Move> candidateMoves(const JobState& /*parent*/) {
    std::vector<Move> moves;
    moves.reserve(BRANCHING);
    for (int i = 0; i < BRANCHING; ++i) moves.push_back({static_cast<uint8_t>(i), 0});
    return moves;
}

void applyMove(JobState& out, const JobState& parent, Move move) {
    out = parent;
    uint8_t slot = parent.boardBytes[7];
    out.boardBytes[slot] = move.insertPoint;
    out.boardBytes[7] = slot + 1;
    out.insertPoint = move.insertPoint;
    out.orientation = move.orientation;
}

bool allBugsEaten(const JobState& state) {
    return state.bugsEatenMask == 0xF;
}

// CPU-side "kernel": no dead moves in this synthetic model, just marks the
// win condition. Kept trivial on purpose -- the interesting logic is in the
// heuristic below, not here.
void evaluateState(JobState& state) {
    uint8_t depth = state.boardBytes[7];
    state.offBoard = 0;
    state.bugsEatenMask =
        (depth == TOTAL_DEPTH && std::memcmp(state.boardBytes, TARGET_PATH, TOTAL_DEPTH) == 0) ? 0xF
                                                                                               : 0;
}

// Lower is better. Rewards matching the DECOY_PATH prefix -- a deliberate
// trap so a pure-greedy beam converges on a dead end instead of the real
// solution, exercising the diversity/restart escape mechanism.
float heuristicScore(const JobState& state) {
    uint8_t depth = state.boardBytes[7];
    int mismatches = 0;
    for (uint8_t i = 0; i < depth; ++i) {
        if (state.boardBytes[i] != DECOY_PATH[i]) ++mismatches;
    }
    return static_cast<float>(mismatches);
}

static JobState makeStart() {
    JobState s{};
    std::memset(&s, 0, sizeof(s));
    return s;
}

int main() {
    // 1. Pure greedy (no diversity, single attempt) should get trapped by
    //    the decoy and fail -- proving the test's decoy is actually a trap,
    //    not something beam search would stumble into a solution for anyway.
    {
        BeamSearchConfig cfg;
        cfg.beamWidth = 1;          // strict hill-climb: only the single
                                    // best-scoring node survives per level
        cfg.diversityFraction4 = 0; // pure greedy
        cfg.restarts = 1;
        std::vector<Move> path;
        size_t nodes = 0;
        bool solved = trySolveHeuristicFirst(makeStart(), cfg, path, &nodes);
        CHECK(!solved); // "pure greedy beam should be trapped by the decoy and fail"
        std::printf("  [greedy-only] solved=%d nodesExplored=%zu (expected to fail)\n", solved,
                    nodes);
    }

    // 2. With diversity + a modest beam + a few restarts, the search should
    //    escape the decoy and find the real solution -- and should do so
    //    exploring only a tiny fraction of the ~6^7 (~280k) full space.
    {
        BeamSearchConfig cfg;
        cfg.beamWidth = 16;
        cfg.diversityFraction4 = 1; // 1/4 of the beam kept for diversity
        cfg.restarts = 20;
        cfg.rngSeed = 12345;
        std::vector<Move> path;
        size_t nodes = 0;
        bool solved = trySolveHeuristicFirst(makeStart(), cfg, path, &nodes);
        CHECK(solved); // "diversified beam search should find the planted solution"
        CHECK(path.size() == TOTAL_DEPTH); // "solution path should have exactly TOTAL_DEPTH moves"

        bool matches = (path.size() == TOTAL_DEPTH);
        for (int i = 0; matches && i < TOTAL_DEPTH; ++i) {
            if (path[i].insertPoint != TARGET_PATH[i]) matches = false;
        }
        CHECK(matches); // "reconstructed path should equal the planted TARGET_PATH"

        size_t fullSpace = 1;
        for (int i = 0; i < TOTAL_DEPTH; ++i) fullSpace *= BRANCHING;
        std::printf(
            "  [diversified] solved=%d nodesExplored=%zu (full space=%zu, %.4f%% explored)\n",
            solved, nodes, fullSpace, 100.0 * (double)nodes / (double)fullSpace);
        CHECK(nodes < fullSpace / 10); // "beam search should explore far less than the full space"
    }

    REPORT();
}
