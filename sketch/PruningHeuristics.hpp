#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <functional>

// ---------------------------------------------------------------------------
// Two board-agnostic pruning heuristics for candidateMoves() generation.
// Both are sound (never discard a candidate that could change the eventual
// win/lose outcome) rather than approximate -- they eliminate genuinely
// redundant candidates, not just "probably not useful" ones.
//
// Neither depends on the real JobState layout -- they operate on small,
// generic types (Cell ids, EntryPoint ids, TileKind) so they can be unit
// tested here and wired into your real candidateMoves() once you have the
// real board/tile encoding. See the usage sketch at the bottom of this file.
// ---------------------------------------------------------------------------

// ===========================================================================
// Heuristic 1: ladybug pre-position deduplication.
//
// Before a given candidate insertion, the ladybug can pre-move to any tile
// in its current reachable set. Partition that reachable set by the line
// (row/column) the candidate insertion is about to shift:
//
//   - Tiles NOT on the shifting line, that are mutually reachable from one
//     another *without the path passing through a tile on the shifting
//     line*, are unaffected by the insertion in every way that matters:
//     the insertion changes none of their tiles or connections, so the
//     resulting reachable set after insertion is identical regardless of
//     which one of them the ladybug pre-positioned to. Only one
//     representative per such group needs to be tried.
//
//   - Tiles ON the shifting line must still be tried individually: the
//     ladybug gets carried along by the shift, and exactly which tile
//     along the line it started on determines where it ends up.
//
// This is a graph reachability question, so it's expressed generically:
// you supply the reachable set, a same-line predicate, and a one-hop
// adjacency function (the same underlying connectivity your reachability
// computation already walks) -- this file just does the component
// bookkeeping.
// ===========================================================================

// Computes connected components of `reachable` cells that are NOT on the
// affected line, using only edges between two off-line cells that are
// directly adjacent (never routing the connectivity check through an
// on-line cell, since that connectivity may itself change post-insertion).
// Returns: one representative cell per off-line component, plus every
// on-line reachable cell individually (untouched/undeduplicated).
//
// Cell must be hashable (works with plain integers/small structs wrapped
// in std::hash, or swap in a dense array-based union-find if Cell is a
// small contiguous index range -- the unordered_map version here favors
// simplicity/genericity over the last bit of performance, since this runs
// once per candidate insertion, not once per node).
template <typename Cell, typename NeighborsFn, typename OnLineFn>
std::vector<Cell> reducePrePositions(const std::vector<Cell>& reachable, NeighborsFn neighbors,
                                     OnLineFn onAffectedLine) {
    std::unordered_set<Cell> reachableSet(reachable.begin(), reachable.end());

    // Union-Find over off-line reachable cells only.
    std::unordered_map<Cell, Cell> parent;
    for (const Cell& c : reachable) {
        if (!onAffectedLine(c)) parent[c] = c;
    }
    std::function<Cell(Cell)> find = [&](Cell x) -> Cell {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]]; // path halving
            x = parent[x];
        }
        return x;
    };
    auto unite = [&](Cell a, Cell b) {
        Cell ra = find(a), rb = find(b);
        if (ra != rb) parent[ra] = rb;
    };

    for (const Cell& c : reachable) {
        if (onAffectedLine(c)) continue;
        for (const Cell& n : neighbors(c)) {
            if (reachableSet.count(n) && !onAffectedLine(n)) unite(c, n);
        }
    }

    std::vector<Cell> result;
    std::unordered_set<Cell> seenRoots;
    for (const Cell& c : reachable) {
        if (onAffectedLine(c)) {
            result.push_back(c); // always kept individually
        } else {
            Cell root = find(c);
            if (seenRoots.insert(root).second)
                result.push_back(c); // first member seen = representative
        }
    }
    return result;
}

// ===========================================================================
// Heuristic 2: skip insertions that would exactly undo the previous one.
//
// If move N inserted at `prevEntry` and the ladybug did NOT end up on the
// other side of that line afterwards (so its position carries no new
// information relative to before move N), and the tile ejected by move N
// is orientation-indifferent (Blank or Cross -- only 1 distinct
// orientation each), then inserting that same tile back in at the
// opposite end of the *same* line (any orientation) exactly restores the
// line -- and hence the whole board -- to its pre-move-N arrangement.
// That candidate is pure waste: skip it during candidateMoves() for move
// N+1.
//
// Deliberately NOT attempted for Straight/Corner/Stub tiles: undoing
// those requires matching the exact prior orientation too, which is a
// real (if rarer) case you may want to add once your orientation
// encoding is in hand -- kept out here to stay strictly conservative.
// ===========================================================================
enum class TileKind : uint8_t { Blank, Cross, Straight, Corner, Stub };

inline bool isOrientationIndifferent(TileKind k) {
    return k == TileKind::Blank || k == TileKind::Cross;
}

// 10 entries modeled as 5 lines x 2 ends: entry e belongs to line (e/2),
// end (e%2); the opposite end of the same line is the other end index.
// Remap to your real entry-point numbering if it differs.
using EntryPoint = uint8_t;
constexpr EntryPoint NUM_ENTRIES = 10;

inline EntryPoint lineOf(EntryPoint e) {
    return e / 2;
}
inline EntryPoint oppositeEntry(EntryPoint e) {
    return (e / 2) * 2 + (1 - e % 2);
}
inline bool sameLine(EntryPoint a, EntryPoint b) {
    return lineOf(a) == lineOf(b);
}

// Returns true if `candidateEntry` (using the tile ejected by the previous
// insertion, of kind `ejectedTileKind`) would exactly undo the previous
// insertion at `prevEntry`, given whether the ladybug ended up on the
// other side of that line during the move following `prevEntry`'s
// insertion. If true, candidateMoves() should omit *all* orientations of
// `candidateEntry` for this tile (they're all equivalent undos, since the
// tile is orientation-indifferent).
inline bool isUndoInsertion(EntryPoint prevEntry, TileKind ejectedTileKind,
                            bool ladybugCrossedToOtherSide, EntryPoint candidateEntry) {
    if (ladybugCrossedToOtherSide) return false;
    if (!isOrientationIndifferent(ejectedTileKind)) return false;
    return candidateEntry == oppositeEntry(prevEntry);
}

// ---------------------------------------------------------------------------
// Usage sketch (not compiled -- illustrates how a real candidateMoves()
// would call into both heuristics; adjust to your real JobState fields):
//
//   std::vector<Move> candidateMoves(const JobState& parent) {
//       std::vector<Move> moves;
//       for (EntryPoint entry = 0; entry < NUM_ENTRIES; ++entry) {
//           if (isUndoInsertion(parent.lastEntry, parent.ejectedTileKind,
//                                parent.ladybugCrossedLastMove, entry)) {
//               continue; // heuristic 2: provably a no-op, skip entirely
//           }
//           auto prePositions = reducePrePositions(
//               parent.ladybugReachableTiles,
//               [&](Cell c) { return boardNeighbors(parent, c); },
//               [&](Cell c) { return isOnLine(entry, c); });
//           for (Cell pos : prePositions) {
//               for (uint8_t orientation = 0;
//                    orientation < orientationCount(tileKindAt(entry));
//                    ++orientation) {
//                   moves.push_back({entry, orientation, pos});
//               }
//           }
//       }
//       return moves;
//   }
// ---------------------------------------------------------------------------
