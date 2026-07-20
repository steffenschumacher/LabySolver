#pragma once

#include "GameRules.hpp"

#include <cstring>
#include <unordered_map>
#include <vector>

namespace laby {

struct StateHash {
    size_t operator()(const CompactBoardState& state) const noexcept {
        // FNV-1a over the stable representation, ignoring depth so reaching the
        // same physical state later cannot improve a minimum-depth search.
        constexpr size_t OFFSET = sizeof(size_t) == 8 ? 1469598103934665603ull : 2166136261u;
        constexpr size_t PRIME = sizeof(size_t) == 8 ? 1099511628211ull : 16777619u;
        size_t hash = OFFSET;
        const auto* bytes = reinterpret_cast<const uint8_t*>(&state);
        for (size_t i = 0; i < sizeof(state); ++i) {
            const uint8_t byte = i == sizeof(state) - 1 ? static_cast<uint8_t>(bytes[i] & 0xc7) : bytes[i];
            hash = (hash ^ byte) * PRIME;
        }
        return hash;
    }
};

struct PhysicalStateEqual {
    bool operator()(const CompactBoardState& a, const CompactBoardState& b) const noexcept {
        if (a.tiles != b.tiles || a.positions != b.positions) return false;
        return (a.progressAndDepth & 0xc7) == (b.progressAndDepth & 0xc7);
    }
};

struct SolveResult {
    bool solved = false;
    bool minimumProven = false;
    uint64_t statesExamined = 0;
    std::vector<Move> moves;
    CompactBoardState finalState{};
};

inline bool replaySolution(const LevelDefinition& level, const std::vector<Move>& moves,
                           CompactBoardState* finalState = nullptr) {
    CompactBoardState state = level.initial;
    state.setDepth(0);
    normalizeGoals(state);
    for (const Move& move : moves) {
        if (state.depth() >= level.maxPushes) return false;
        CompactBoardState child;
        if (!applyCandidate(child, state, move, level.mayPushPlayerOut)) return false;
        state = child;
    }
    if (finalState) *finalState = state;
    return state.won();
}

inline SolveResult solveMinimum(const LevelDefinition& level) {
    validateLevel(level);
    struct Node { CompactBoardState state; size_t parent; Move move; };
    constexpr size_t ROOT = static_cast<size_t>(-1);
    std::vector<Node> nodes;
    CompactBoardState initial = level.initial;
    initial.setDepth(0);
    normalizeGoals(initial);
    nodes.push_back(Node{initial, ROOT, {}});
    SolveResult result;
    if (initial.won()) {
        result.solved = result.minimumProven = true;
        result.statesExamined = 1;
        result.finalState = initial;
        return result;
    }
    std::unordered_map<CompactBoardState, size_t, StateHash, PhysicalStateEqual> seen;
    seen.emplace(initial, 0);
    size_t layerBegin = 0, layerEnd = 1;
    for (uint8_t depth = 0; depth < level.maxPushes; ++depth) {
        const size_t nextBegin = nodes.size();
        for (size_t parentIndex = layerBegin; parentIndex < layerEnd; ++parentIndex) {
            // nodes.push_back below may reallocate; keep a value copy rather
            // than a reference into the vector across child insertion.
            const CompactBoardState parentState = nodes[parentIndex].state;
            for (const Move& move : candidateMoves(parentState, level.mayPushPlayerOut)) {
                CompactBoardState child;
                if (!applyCandidate(child, parentState, move, level.mayPushPlayerOut)) continue;
                ++result.statesExamined;
                if (seen.find(child) != seen.end()) continue;
                const size_t childIndex = nodes.size();
                nodes.push_back(Node{child, parentIndex, move});
                seen.emplace(child, childIndex);
                if (child.won()) {
                    result.solved = result.minimumProven = true;
                    result.finalState = child;
                    for (size_t i = childIndex; nodes[i].parent != ROOT; i = nodes[i].parent)
                        result.moves.push_back(nodes[i].move);
                    std::reverse(result.moves.begin(), result.moves.end());
                    return result;
                }
            }
        }
        layerBegin = nextBegin;
        layerEnd = nodes.size();
        if (layerBegin == layerEnd) break;
    }
    result.minimumProven = true;
    return result;
}

} // namespace laby
