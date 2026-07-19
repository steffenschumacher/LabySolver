# Pruning Heuristics

`PruningHeuristics.hpp` implements two board-agnostic rules for reducing
the number of branches the search needs to try, without sacrificing
exhaustiveness (both are *sound* reductions — they only ever remove
provably-redundant or provably-pointless branches, never a branch that
could matter). Neither is currently wired into a real `candidateMoves()`
implementation — see the commented usage sketch at the bottom of the
header, and `docs/STATUS.md`.

## Heuristic 1 — ladybug pre-position deduplication (`reducePrePositions`)

**The idea**: before inserting the spare tile at some entry point, the
ladybug may be able to reach several different tiles first (recall from
`docs/OVERVIEW.md` that the ladybug can reposition itself before an
insertion). If two of those reachable tiles are connected to each other
*without* passing through any tile on the row/column about to be shifted,
then inserting the tile doesn't change anything about the connectivity
between them — so trying the insertion from either position is
guaranteed to produce the exact same resulting board. There's no need to
try both as separate candidate moves; only one representative is needed.

Tiles that lie *on* the line about to shift are different: the
insertion **will** change their state directly, so each one must still
be tried individually — they can never be merged away.

### Why this is sound, not just approximate

The tempting-but-wrong version of this idea would be "group reachable
cells by whatever component they fall into, using the *whole* current
board's connectivity." That's unsound: two off-line cells might only be
mutually reachable *through* an on-line cell right now — and since the
insertion is about to change that on-line cell, their post-insertion
outcomes could actually differ. `reducePrePositions()` avoids this trap
by building connectivity **only from edges between two off-line
cells** — any path that has to detour through an on-line cell doesn't
count as a merge. `tests/test_pruning_reduce_positions.cpp` has an
explicit test case (`case2`, the "trap" case) built specifically to
catch a naive/unsound implementation that ignores this distinction.

### Interface

```cpp
template <typename Cell, typename NeighborsFn, typename OnLineFn>
std::vector<Cell> reducePrePositions(
    const std::vector<Cell>& reachable,   // all tiles the ladybug can reach right now
    NeighborsFn neighbors,                // Cell -> std::vector<Cell> (board adjacency)
    OnLineFn onAffectedLine                // Cell -> bool (is this tile on the row/col about to shift?)
);
```

Generic over whatever `Cell` type you use to identify a board tile
(coordinates, an index, whatever) — it only needs to be hashable
(used as an `unordered_map` key internally for the union-find). Returns:
one representative per off-line connected component, plus every
on-line reachable cell individually (never merged).

Implementation: union-find (disjoint-set) over the reachable set, unioning
only pairs `(a, b)` where `neighbors(a)` contains `b`, both `a` and `b`
are reachable, and *neither* is on the affected line.

## Heuristic 2 — undo-move skipping (`isUndoInsertion`)

**The idea**: every line (row or column) has two ends/entry points. If
you insert the spare tile at one end, and then (without the ladybug
having crossed to the far side of that line in between) your *next* move
inserts a tile back in at the *opposite* end of the *same* line — and the
tile that got pushed out by the first insertion has only one meaningful
orientation (a `Blank` or a `Cross` — see `docs/OVERVIEW.md`'s tile-kind
list), so orientation choice doesn't matter — then that second insertion
exactly restores the board to what it looked like *before* the first
insertion. It's a provable no-op: a wasted move that undoes the previous
one and gains nothing. Such candidate moves should simply never be
generated in the first place.

### Why it's scoped to just Blank/Cross tiles

If the tile involved were a `Straight`, `Corner`, or `Stub` (2 or 4
distinct orientations), reinserting it at the opposite end only restores
the original board if you also happen to pick the exact matching prior
orientation — which `isUndoInsertion()` doesn't currently check. Rather
than risk a false positive (skipping a move that *isn't* actually a
no-op), the heuristic deliberately stays conservative and only ever
fires for orientation-indifferent tiles, where there's no orientation
subtlety to get wrong. Extending it to also match exact orientations for
Straight/Corner/Stub tiles is a possible future improvement, not
attempted yet (see `docs/STATUS.md`).

### Why "the ladybug crossed to the other side" matters

Even if the *board* would end up byte-for-byte identical to before the
first insertion, the *ladybug's position* might not be — if it moved to
the other side of that line in between the two insertions, the overall
game state (board + ladybug position) is not actually a no-op, so the
move must not be skipped.

### Interface

```cpp
enum class TileKind { Blank, Cross, Straight, Corner, Stub };
bool isOrientationIndifferent(TileKind);

using EntryPoint = uint8_t;             // 0..9, 10 entries = 5 lines x 2 ends
constexpr size_t NUM_ENTRIES = 10;
int lineOf(EntryPoint);                  // which of the 5 lines this entry belongs to
EntryPoint oppositeEntry(EntryPoint);    // the other end of the same line
bool sameLine(EntryPoint a, EntryPoint b);

bool isUndoInsertion(
    EntryPoint prevEntry,               // where the previous insertion happened
    TileKind ejectedTileKind,            // the tile kind that insertion pushed out
    bool ladybugCrossedToOtherSide,       // did the ladybug cross this line since then?
    EntryPoint candidateEntry            // the entry point being considered now
);
```

`isUndoInsertion` returns true (skip this candidate) only when all of:
`candidateEntry == oppositeEntry(prevEntry)`, `isOrientationIndifferent
(ejectedTileKind)`, and `!ladybugCrossedToOtherSide`.

## Testing

- `tests/test_pruning_reduce_positions.cpp` — hand-built small graph with
  clearly separate off-line components, the deliberate "bridge-through-
  an-on-line-cell" trap case (must NOT merge), an isolated on-line cell
  (must always be kept individually), and edge cases (empty input,
  already-disjoint cells).
- `tests/test_pruning_undo_move.cpp` — table-driven, includes an
  exhaustive sweep over every `(prevEntry, tileKind, crossed)`
  combination confirming exactly one candidate entry is ever flagged,
  and only under the documented conditions.

Both pass; see `docs/BUILD_AND_TEST.md` to run them yourself.

## Not yet done

- Neither heuristic is wired into a real, board-specific
  `candidateMoves()` implementation — `PruningHeuristics.hpp` has a
  commented (non-compiled) usage sketch near the bottom of the file
  showing the intended call pattern, but there's no real board logic to
  call it from yet in this repository.
- Heuristic 2 doesn't attempt orientation-aware undo-detection for
  Straight/Corner/Stub tiles (see above) — flagged as a possible future
  extension, not started.
