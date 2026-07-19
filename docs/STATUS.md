# Status: what's done, what's stubbed, what's next

Last updated: this reflects the state after the SeedQueue-abort deadlock
fix and the chunk-counting fairness-test fix were both verified (10+
repeated clean runs each), and after `PruningHeuristics.hpp` was written
and tested. If you're a new agent/session picking this up, read this
file first, then dip into `docs/ARCHITECTURE.md` for the "why".

## Done and tested (green, verified with repeated runs)

- **Chain/NodePool** (`Chain.hpp`, `NodePool.hpp`): intrusive linked
  list + arena allocator. Tested (`test_chain`, `test_nodepool`).
- **Inbox** (`Inbox.hpp`): lock-free SPSC ring buffer of `Chain`
  descriptors. Tested (`test_inbox`).
- **SeedQueue** (`SeedQueue.hpp`): bounded blocking master->worker
  queue, now with a working `abort()` mechanism (see below). Tested
  (`test_seedqueue`, and exercised for real in `test_end_to_end_sim`).
- **Dispatcher** (`Dispatcher.hpp`): round-robin fair batching across
  producers. Tested (`test_dispatcher_fairness`, `test_end_to_end_sim`).
- **Master/Worker DFS** (`Master.hpp`, `Worker.hpp`): explicit-stack
  depth-first search, `MASTER_DEPTH = 4` (master owns moves 1-4, workers
  own 5-7). Tested end-to-end (`test_end_to_end_sim`), including
  solution-path reconstruction via `parent` pointers + seed move lists.
- **Two concurrency bugs found and fixed** (both were real deadlocks/
  hangs, not test artifacts — see `docs/ARCHITECTURE.md` for full
  writeups):
  1. `SeedQueue` push/pop were unaware of `SearchGlobals::solutionFound`,
     causing a real deadlock when a solution was found while the master
     was blocked pushing to a full queue. Fixed via `SeedQueue::abort()`.
  2. `tryExpand` (in both `Master.hpp` and `Worker.hpp`, plus the
     original copy in `test_dispatcher_fairness.cpp`) tracked submit/
     collect completion by counting **calls**, which breaks once the
     dispatcher merges multiple chunks into one result chain under
     backlog — fixed by counting total **nodes** instead.
  Both fixes were verified via 10-15 repeated runs under `timeout`, not
  just a single passing run (see `docs/BUILD_AND_TEST.md`'s note on why
  that matters for this codebase specifically).
- **BeamSearch** (`BeamSearch.hpp`): heuristic-first, non-exhaustive
  CPU-only pre-pass. Tested against a synthetic adversarial board
  (`test_beam_search`) — confirmed diversity injection + restarts
  recover from a planted local-optimum trap that defeats pure greedy
  search. See `docs/BEAM_SEARCH.md` for a full plain-language
  explanation.
- **PruningHeuristics** (`PruningHeuristics.hpp`): two board-agnostic
  search-space reduction rules (`reducePrePositions`, `isUndoInsertion`),
  both tested (`test_pruning_reduce_positions`, `test_pruning_undo_move`)
  including a deliberate "trap" test case for heuristic 1's soundness.
  See `docs/PRUNING_HEURISTICS.md`.

## Explicitly NOT done / stubbed out — the real work still ahead

These are the actual highest-value next steps, roughly in the order
they'd naturally come up:

1. **The real board model doesn't exist yet.** `JobState.hpp` is a
   placeholder (`boardBytes[64]` + a few result fields). None of
   `candidateMoves()`, `applyMove()`, `allBugsEaten()` are implemented —
   they're declared (so the header-only scaffolding compiles) but never
   defined. This is the single largest remaining piece of work: encoding
   the actual 5x7 board, 10 tile-insertion entries, tile
   kinds/orientations, ladybug/bug positions, and reachability logic.

2. **The CUDA kernel doesn't exist in this repo.** `launchCudaBatch()`
   is declared, never defined (except trivial fakes inside the test
   files). The user mentioned an existing CUDA kernel from prior work
   (per the original project description) that computes reachability/
   eaten-bugs and a compact resulting-board representation — that
   kernel needs to be adapted to this `JobState` layout (or vice versa:
   adapt `JobState` to match the kernel's existing I/O format) and
   linked in for real.

3. **`PruningHeuristics.hpp`'s two heuristics are not wired into any
   real `candidateMoves()`.** They're implemented and tested in
   isolation, with only a commented (non-compiled) usage sketch showing
   the intended call pattern. Once a real `candidateMoves()` exists (see
   #1), both heuristics should be called from it to cut down the
   ~150/move branching factor before jobs are ever submitted to the GPU.

4. **`BeamSearch.hpp` is not wired into the main pipeline.** The
   recommended composition (try beam search first, fall back to the full
   `Master`/`Worker`/`Dispatcher` brute force only if it fails) is
   described in `docs/BEAM_SEARCH.md` and `docs/OVERVIEW.md` but not
   implemented in `main_sketch.cpp` or anywhere else — it currently only
   has its own standalone test against a synthetic board.

5. **`main_sketch.cpp` is a wiring sketch, not a runnable program.** It
   shows how `NodePool`/`Dispatcher`/`Master`/`Worker`/`SeedQueue` connect
   and get spun up as threads, but can't compile/link/run as-is until
   items #1-#2 above exist. Also missing: actually reading a level's
   starting board (currently `JobState initialBoard{};` — all zeros) and
   printing/using the reconstructed solution path once found (there's a
   comment marking where that logic goes, but no code).

6. **`Master.hpp`/`Worker.hpp` share near-duplicate DFS logic**
   (`Frame`, `tryExpand`, `runStack` are structurally identical except
   for how each decides "keep expanding vs. hand off/stop"). This was a
   deliberate, acknowledged simplification during the DFS rewrite —
   worth factoring into a shared template/base at some point, but not
   urgent (both are independently tested and correct as-is).

7. **`PruningHeuristics.hpp`'s undo-detection (heuristic 2) is
   deliberately conservative**: it only fires for orientation-indifferent
   tiles (Blank/Cross). Extending it to also detect undos for Straight/
   Corner/Stub tiles (which would require matching the exact prior
   orientation, not just tile kind) is a possible future improvement,
   not started. See `docs/PRUNING_HEURISTICS.md` for why.

8. **`heuristicScore()` and `evaluateState()` (BeamSearch's board hooks)
   have never been tuned or tested against the real game** — only
   against the synthetic test board. Once the real board model exists
   (#1), these need real implementations and will likely need iteration
   to get beam search's success rate up on actual levels.

9. **No handling yet for "no solution exists within the move budget."**
   Both the exhaustive brute force and beam search can, correctly,
   report failure — but there's no guidance yet in this codebase for
   what a caller should do with that (e.g. relax the move budget and
   retry, report to the player that the level is unsolvable as
   configured, etc.). Likely a product/UX decision more than a code one,
   just flagging it as unaddressed.

## Known environment quirks (not project bugs, just local friction)

- The Makefile has a macOS-specific `-isystem` workaround for a broken
  standalone Xcode Command Line Tools install — see
  `docs/BUILD_AND_TEST.md`'s note; harmless/no-op on a normal Linux/CUDA
  build machine.
- This environment's shell disallows `kill $PID`-style variable
  substitution in a single invocation for safety reasons — use a
  literal numeric PID (`kill -9 1234`) in its own command instead, if
  you ever need to clean up a hung test process.

## Suggested order of work for whoever picks this up next

1. Nail down the real `JobState` encoding + `candidateMoves`/
   `applyMove`/`allBugsEaten` against the actual game rules (item #1).
   This unblocks everything else.
2. Wire in the real CUDA kernel (item #2) — even a naive/slow first
   version is enough to validate the whole pipeline end-to-end on a
   real (not synthetic) board.
3. Run `test_end_to_end_sim`-style validation against a real, small,
   known-solvable level to confirm the whole stack produces a correct
   answer, before scaling up to the full 7-move level this project was
   originally motivated by.
4. Wire in `PruningHeuristics.hpp` (item #3) and re-measure — this is
   where a meaningful chunk of the ~150/move branching factor should
   get cut down.
5. Wire in `BeamSearch.hpp` as a pre-pass (item #4) and tune its
   heuristic — likely the highest-leverage remaining item for making
   the *typical* case fast, even though the exhaustive path (with
   pruning) remains the correctness backstop.
