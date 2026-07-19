#pragma once
#include "JobState.hpp"
#include "Worker.hpp" // reuses Move / candidateMoves / applyMove / allBugsEaten

#include <vector>
#include <algorithm>
#include <random>
#include <cstdint>

// ---------------------------------------------------------------------------
// Heuristic-first search: a cheap, CPU-only, non-exhaustive alternative to
// the full GPU brute-force pipeline. Intended use:
//
//     std::vector<Move> path;
//     if (trySolveHeuristicFirst(start, {}, path)) { ... done, no GPU needed ... }
//     else { fall back to Master/Worker/Dispatcher brute force }
//
// Rationale: total node count here is beamWidth * ~30 candidates * depth,
// e.g. 64 * 30 * 7 ~= 13k evaluations for the whole search -- three to five
// orders of magnitude below anything that needs GPU batching. So this stays
// single-threaded, synchronous, and allocates a handful of small vectors
// instead of using the NodePool/Chain/Dispatcher machinery at all. It trades
// completeness (may fail to find a solution that exists) for speed, and is
// meant to run BEFORE the brute-force path, not replace it.
// ---------------------------------------------------------------------------

// Board-specific hook #1 (new, not needed by the brute-force pipeline):
// synchronously fill reachableMask / bugsEatenMask / offBoard for a single
// state on the CPU. This is the same computation the CUDA kernel does per
// job -- either port that logic directly, or (if that's inconvenient) call
// your existing single-job code path / launchCudaBatch(&state, 1). Batches
// here are tiny (beamWidth * ~30), so a CPU port is almost certainly faster
// in practice than round-tripping to the GPU at this volume.
void evaluateState(JobState& state);

// Board-specific hook #2: lower is better. Should reward eating more bugs
// and being close (e.g. reachable-graph distance, not just Manhattan) to
// whichever uneaten bug is nearest. Zero real intelligence is assumed here
// beyond "this function tells us which of several boards is more promising";
// tune it against your actual bug/board encoding.
float heuristicScore(const JobState& state);

struct BeamSearchConfig {
    size_t beamWidth = 64;         // candidates kept per level
    size_t maxDepth = 7;           // matches the real move budget
    size_t diversityFraction4 = 1; // 1/4th of the beam is kept "random" (see below), 0 disables
    size_t restarts = 8;           // independent attempts with different RNG shuffles
    uint32_t rngSeed = 0xC0FFEE;
};

struct BeamNode {
    JobState state;
    std::vector<Move> path; // trivial memory footprint: <= maxDepth entries
    float score = 0.0f;
};

namespace beam_detail {

// Expands every node in `beam` by all candidate moves, evaluates each child,
// and returns the full expanded set (not yet trimmed). Small enough
// (beamWidth * ~30) to just build in a plain vector.
inline std::vector<BeamNode> expandAll(const std::vector<BeamNode>& beam) {
    std::vector<BeamNode> children;
    children.reserve(beam.size() * 32);
    for (const BeamNode& parent : beam) {
        for (Move m : candidateMoves(parent.state)) {
            BeamNode child;
            applyMove(child.state, parent.state, m);
            evaluateState(child.state);
            if (child.state.offBoard) continue; // dead move, don't even score it
            child.path = parent.path;
            child.path.push_back(m);
            child.score = heuristicScore(child.state);
            children.push_back(std::move(child));
        }
    }
    return children;
}

// Keeps the best `beamWidth` nodes by score, plus (optionally) a random
// slice of the remainder for diversity -- a light touch of genetic-algorithm
// style exploration so the search doesn't collapse onto one greedy path and
// get stuck behind a local optimum the heuristic can't see past.
inline std::vector<BeamNode> selectSurvivors(std::vector<BeamNode>&& children,
                                             const BeamSearchConfig& cfg, std::mt19937& rng) {
    if (children.size() <= cfg.beamWidth) return std::move(children);

    size_t eliteCount = cfg.beamWidth;
    size_t randomCount = 0;
    if (cfg.diversityFraction4 > 0) {
        randomCount = cfg.beamWidth / (cfg.diversityFraction4 + 1);
        eliteCount = cfg.beamWidth - randomCount;
    }

    std::nth_element(children.begin(), children.begin() + eliteCount, children.end(),
                     [](const BeamNode& a, const BeamNode& b) { return a.score < b.score; });

    std::vector<BeamNode> survivors;
    survivors.reserve(cfg.beamWidth);
    for (size_t i = 0; i < eliteCount; ++i) survivors.push_back(std::move(children[i]));

    if (randomCount > 0) {
        std::shuffle(children.begin() + eliteCount, children.end(), rng);
        size_t take = std::min(randomCount, children.size() - eliteCount);
        for (size_t i = 0; i < take; ++i) survivors.push_back(std::move(children[eliteCount + i]));
    }
    return survivors;
}

// Single beam-search attempt from `start`. Returns true + fills `outPath` on
// success. `rng` is threaded through so repeated attempts (restarts) explore
// different tie-breaks/diversity picks instead of repeating the same run.
inline bool runOnce(const JobState& start, const BeamSearchConfig& cfg, std::mt19937& rng,
                    std::vector<Move>& outPath, size_t& nodesExplored) {
    std::vector<BeamNode> beam;
    beam.push_back(BeamNode{start, {}, heuristicScore(start)});

    for (size_t depth = 0; depth < cfg.maxDepth; ++depth) {
        std::vector<BeamNode> children = expandAll(beam);
        nodesExplored += children.size();

        for (BeamNode& c : children) {
            if (allBugsEaten(c.state)) {
                outPath = std::move(c.path);
                return true;
            }
        }
        if (children.empty()) return false; // every branch this level was a dead end

        beam = selectSurvivors(std::move(children), cfg, rng);
    }
    return false;
}

} // namespace beam_detail

// Runs up to `cfg.restarts` independent beam-search attempts (cheap: each is
// already a small fraction of a second's worth of work) before giving up.
// Returns true and fills outPath if any attempt finds a full solution.
inline bool trySolveHeuristicFirst(const JobState& start, BeamSearchConfig cfg,
                                   std::vector<Move>& outPath,
                                   size_t* totalNodesExplored = nullptr) {
    std::mt19937 rng(cfg.rngSeed);
    size_t nodes = 0;
    for (size_t attempt = 0; attempt < cfg.restarts; ++attempt) {
        if (beam_detail::runOnce(start, cfg, rng, outPath, nodes)) {
            if (totalNodesExplored) *totalNodesExplored = nodes;
            return true;
        }
        // Reseed so the next attempt's diversity-slice shuffles differ,
        // rather than every restart being identical (pure greedy beam
        // search is deterministic otherwise).
        rng.discard(1);
    }
    if (totalNodesExplored) *totalNodesExplored = nodes;
    return false;
}
