#pragma once

#include "BoardState.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace laby {

struct LevelDefinition {
    CompactBoardState initial{};
    uint8_t maxPushes = 0;
    bool mayPushPlayerOut = false;
};

struct Insertion {
    uint8_t x;
    uint8_t y;
    int8_t dx;
    int8_t dy;
};

inline constexpr std::array<Insertion, 10> INSERTIONS{{
    {0, 1, 1, 0}, {0, 3, 1, 0}, {0, 5, 1, 0},
    {1, 0, 0, 1}, {3, 0, 0, 1},
    {1, 6, 0, -1}, {3, 6, 0, -1},
    {4, 1, -1, 0}, {4, 3, -1, 0}, {4, 5, -1, 0},
}};

struct Move {
    uint8_t insertionPoint = 0;
    uint8_t rotations = 0;
    uint8_t ladybugPrePosition = 0;

    friend bool operator==(const Move& a, const Move& b) {
        return a.insertionPoint == b.insertionPoint && a.rotations == b.rotations &&
               a.ladybugPrePosition == b.ladybugPrePosition;
    }
};

inline uint8_t oppositeOpening(uint8_t opening) {
    return opening == North ? South : opening == East ? West : opening == South ? North : East;
}

inline uint64_t reachableCells(const CompactBoardState& state, uint8_t start) {
    if (start >= BOARD_CELLS) return 0;
    uint64_t reached = uint64_t{1} << start;
    std::array<uint8_t, BOARD_CELLS> queue{};
    size_t head = 0, tail = 0;
    queue[tail++] = start;
    constexpr std::array<int8_t, 4> DX{{0, 1, 0, -1}};
    constexpr std::array<int8_t, 4> DY{{-1, 0, 1, 0}};
    constexpr std::array<uint8_t, 4> OPEN{{North, East, South, West}};
    while (head < tail) {
        const uint8_t from = queue[head++];
        const int x = from % BOARD_WIDTH;
        const int y = from / BOARD_WIDTH;
        const uint8_t mask = state.tile(from);
        for (size_t d = 0; d < 4; ++d) {
            if (!(mask & OPEN[d])) continue;
            const int nx = x + DX[d], ny = y + DY[d];
            if (nx < 0 || nx >= BOARD_WIDTH || ny < 0 || ny >= BOARD_HEIGHT) continue;
            const uint8_t to = cellIndex(static_cast<uint8_t>(nx), static_cast<uint8_t>(ny));
            if (!(state.tile(to) & oppositeOpening(OPEN[d]))) continue;
            const uint64_t bit = uint64_t{1} << to;
            if (!(reached & bit)) {
                reached |= bit;
                queue[tail++] = to;
            }
        }
    }
    return reached;
}

inline uint64_t normalizeGoals(CompactBoardState& state) {
    const uint64_t reached = reachableCells(state, state.ladybug());
    uint8_t next = state.nextGoal();
    const uint8_t count = state.goalCount();
    while (next < count) {
        const uint8_t position = state.goal(next);
        if (position >= BOARD_CELLS || !(reached & (uint64_t{1} << position))) break;
        ++next;
    }
    state.setNextGoal(next);
    return reached;
}

inline void validateLevel(const LevelDefinition& level) {
    if (level.maxPushes > 7) throw std::invalid_argument("push allowance exceeds encoding");
    if (level.initial.ladybug() > SPARE_POSITION) throw std::invalid_argument("invalid ladybug");
    const uint8_t count = level.initial.goalCount();
    if (count == 0 || count > 4) throw std::invalid_argument("level requires one to four goals");
    bool unused = false;
    for (uint8_t i = 0; i < 4; ++i) {
        const uint8_t p = level.initial.goal(i);
        if (p == UNUSED_GOAL_POSITION) unused = true;
        else if (p > SPARE_POSITION || unused) throw std::invalid_argument("invalid ordered goal slots");
    }
}

inline uint8_t shiftedPosition(uint8_t position, const Insertion& in) {
    if (position == SPARE_POSITION) return cellIndex(in.x, in.y);
    if (position >= BOARD_CELLS) return position;
    const int x = position % BOARD_WIDTH;
    const int y = position / BOARD_WIDTH;
    const bool onLine = in.dx != 0 ? y == in.y : x == in.x;
    if (!onLine) return position;
    const int nx = x + in.dx, ny = y + in.dy;
    if (nx < 0 || nx >= BOARD_WIDTH || ny < 0 || ny >= BOARD_HEIGHT) return SPARE_POSITION;
    return cellIndex(static_cast<uint8_t>(nx), static_cast<uint8_t>(ny));
}

inline bool applyMove(CompactBoardState& out, const CompactBoardState& parent, const Move& move,
                      bool mayPushPlayerOut) {
    if (move.insertionPoint >= INSERTIONS.size() || move.rotations > 3 ||
        move.ladybugPrePosition > SPARE_POSITION)
        return false;
    if (parent.ladybug() == SPARE_POSITION) {
        if (move.ladybugPrePosition != SPARE_POSITION) return false;
    } else {
        const uint64_t reachable = reachableCells(parent, parent.ladybug());
        if (move.ladybugPrePosition >= BOARD_CELLS ||
            !(reachable & (uint64_t{1} << move.ladybugPrePosition))) return false;
    }

    out = parent;
    out.setLadybug(move.ladybugPrePosition);
    const Insertion& in = INSERTIONS[move.insertionPoint];
    uint8_t incoming = parent.tile(SPARE_POSITION);
    for (uint8_t r = 0; r < move.rotations; ++r) incoming = rotateClockwise(incoming);

    int x = in.x, y = in.y;
    uint8_t carried = incoming;
    while (x >= 0 && x < BOARD_WIDTH && y >= 0 && y < BOARD_HEIGHT) {
        const uint8_t index = cellIndex(static_cast<uint8_t>(x), static_cast<uint8_t>(y));
        const uint8_t displaced = parent.tile(index);
        out.setTile(index, carried);
        carried = displaced;
        x += in.dx;
        y += in.dy;
    }
    out.setTile(SPARE_POSITION, carried);

    for (uint8_t slot = 0; slot < POSITION_COUNT; ++slot)
        out.setPosition(slot, shiftedPosition(out.position(slot), in));
    if (!mayPushPlayerOut && out.ladybug() == SPARE_POSITION) return false;
    out.setDepth(static_cast<uint8_t>(parent.depth() + 1));
    normalizeGoals(out);
    return true;
}

inline std::vector<uint8_t> distinctRotations(uint8_t tileMask) {
    std::vector<uint8_t> result;
    uint8_t mask = tileMask;
    for (uint8_t r = 0; r < 4; ++r) {
        if (std::find(result.begin(), result.end(), mask) == result.end()) result.push_back(mask);
        mask = rotateClockwise(mask);
    }
    return result;
}

inline std::vector<Move> candidateMoves(const CompactBoardState& state, bool mayPushPlayerOut) {
    std::vector<Move> moves;
    if (state.ladybug() == SPARE_POSITION) {
        // It can only return by inserting its tile; its pre-position is encoded
        // as the entry cell after the insertion, handled as a special case below.
    }
    const uint64_t reachable = reachableCells(state, state.ladybug());
    std::vector<uint8_t> rotations;
    uint8_t mask = state.tile(SPARE_POSITION);
    for (uint8_t r = 0; r < 4; ++r) {
        if (std::find(rotations.begin(), rotations.end(), mask) == rotations.end()) rotations.push_back(mask);
        mask = rotateClockwise(mask);
    }
    for (uint8_t insertion = 0; insertion < INSERTIONS.size(); ++insertion) {
        for (uint8_t r = 0; r < rotations.size(); ++r) {
            if (state.ladybug() == SPARE_POSITION) {
                moves.push_back(Move{insertion, r, SPARE_POSITION});
                continue;
            }
            for (uint8_t p = 0; p < BOARD_CELLS; ++p) {
                if (!(reachable & (uint64_t{1} << p))) continue;
                if (!mayPushPlayerOut && shiftedPosition(p, INSERTIONS[insertion]) == SPARE_POSITION)
                    continue;
                moves.push_back(Move{insertion, r, p});
            }
        }
    }
    return moves;
}

inline bool applyCandidate(CompactBoardState& out, const CompactBoardState& parent, const Move& move,
                           bool mayPushPlayerOut) {
    return applyMove(out, parent, move, mayPushPlayerOut);
}

} // namespace laby
