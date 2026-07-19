# Overview: the game and the search problem

## The game

The board is 5x7 tiles. Each tile is one of:

- **Cross** — connects all 4 sides (1 orientation; rotating it does nothing).
- **Straight** — connects 2 opposite sides (2 distinct orientations: horizontal/vertical).
- **Corner** — connects 2 adjacent sides (4 distinct orientations).
- **Stub** — connects to only 1 side, a dead end (4 distinct orientations).
- **Blank** — connects to nothing (1 orientation).

A ladybug token sits on one tile. Four bug tokens sit on four other tiles.
The player must walk the ladybug, via the paths currently connected
between tiles, to each of the 4 bugs in turn (order may or may not be
fixed depending on the level — check the specific level's rules), within
a strict number of moves (e.g. 7 for the level driving this project).

Only the **even** rows and columns are movable as whole strips. There is
always exactly one "spare" tile not currently on the board. On your move,
you insert the spare tile into one end of any movable row/column; this
pushes every tile in that strip along by one, and the tile that falls off
the opposite end becomes the new spare tile. You choose the spare tile's
**orientation** before inserting it (subject to how many distinct
orientations that tile kind actually has — see above).

With 5 columns and 7 rows, the even lines give **10 insertion points**
(5 columns x 2 ends + ... see the actual level's board for the exact
count used in this project — treat 10 as the working assumption baked
into `SeedQueue.hpp`'s `EntryPoint`/`NUM_ENTRIES` and
`PruningHeuristics.hpp`).

Critically: **the ladybug can also move on its own**, both before and
after a tile insertion, as long as connected paths allow it. This means a
single "move" in the turn-budget sense is really:

1. (optional) move the ladybug along currently-connected paths,
2. insert the spare tile at one entry, in one orientation,
3. (optional) move the ladybug again along the *new* connectivity,
   potentially eating any bug now reachable.

## Why this needs brute force at all

Each full "move" (steps 1-3 above) has roughly:

- ~10 insertion points x up to 4 orientations = up to ~30-40
  tile-insertion combinations, times
- ~5 (average) distinct ladybug pre-insertion positions that lead to
  meaningfully different outcomes,

giving a **branching factor of roughly 150 per move** once the ladybug's
own repositioning is folded in. Over a 7-move budget that's
`150^7` ≈ 1.7 x 10^15 raw leaves in the naive worst case — obviously
intractable to enumerate literally, but heavy pruning (see
`docs/PRUNING_HEURISTICS.md`), the move budget being much smaller than
that exponent suggests in practice, and GPU-parallel batch evaluation
make an *exhaustive* search over the real, pruned branching factor
tractable within reasonable time and memory — provided the CPU side
doesn't blow up its own memory while feeding the GPU. That CPU-side
design is what this repository is about; see `docs/ARCHITECTURE.md`.

## Two solving strategies in this repo

1. **Exhaustive brute force** (`Master.hpp` + `Worker.hpp` +
   `Dispatcher.hpp` + `SeedQueue.hpp` + `NodePool.hpp`): guaranteed to
   find a solution if one exists within the move budget, memory-bounded
   via depth-first search. This is the primary, load-bearing design.
   See `docs/ARCHITECTURE.md`.

2. **Heuristic-first pre-pass** (`BeamSearch.hpp`): a much cheaper,
   non-exhaustive CPU-only search that often finds a solution in a tiny
   fraction of the time/memory, tried *before* falling back to the full
   brute force. Not guaranteed to succeed. See `docs/BEAM_SEARCH.md`.

Recommended pipeline order for a real solver: try `BeamSearch.hpp` first
(cheap, fast, frequently succeeds); if it reports failure, fall back to
the full `Master`/`Worker`/`Dispatcher` brute-force pipeline (slower,
but exhaustive/guaranteed within the move budget). This composition is
not yet wired up in code — see `docs/STATUS.md`.
