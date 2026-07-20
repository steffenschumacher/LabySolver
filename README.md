# LabySolver

A brute-force + heuristic solver for a sliding-tile maze puzzle (the
"Labymania"-style iPhone game): a 5x7 board of path tiles, a ladybug that
must reach one to four ordered insects, and a single spare tile inserted along
movable rows/columns to reshape the maze between moves. The player (and
this solver) has a strict level-specific budget of two to seven pushes.

This repository contains a working canonical CPU board model and reference
solver plus the tested, memory-bounded search/dispatch architecture. The real
CUDA reachability kernel remains to be implemented; the dispatcher currently
accepts mocked CUDA evaluators for concurrency and performance testing.

## Start here

- **[`docs/OVERVIEW.md`](docs/OVERVIEW.md)** — the game rules, the search
  problem, and why this needs a GPU+CPU pipeline at all. Read this first.
- **[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)** — the full host-side
  design: node pool, chains, dispatcher, master/worker DFS split, seed
  queue. This is the main technical reference.
- **[`docs/BEAM_SEARCH.md`](docs/BEAM_SEARCH.md)** — the optional
  heuristic-first pre-pass (plain-language explanation + how it works).
- **[`docs/PRUNING_HEURISTICS.md`](docs/PRUNING_HEURISTICS.md)** — two
  board-agnostic search-space reduction rules.
- **[`docs/STATUS.md`](docs/STATUS.md)** — what's implemented, what's
  stubbed out, known issues, and next steps. **Read this before making
  changes** — it's the most up-to-date "what's actually done" record.
- **[`docs/BUILD_AND_TEST.md`](docs/BUILD_AND_TEST.md)** — how to build
  and run the test suite.
- **[`docs/SIMULATION.md`](docs/SIMULATION.md)** — configurable synthetic
  CUDA/workload simulation for measuring CPU-pipeline feasibility.
- **[`docs/DISTRIBUTED_ARCHITECTURE.md`](docs/DISTRIBUTED_ARCHITECTURE.md)**
  — seed-level distribution across remote GPU hosts.
- **[`docs/INSTRUMENTATION.md`](docs/INSTRUMENTATION.md)** — per-depth and
  per-seed measurements used to estimate total search-tree size.
- **[`docs/ASYNC_WORKER_SCHEDULER.md`](docs/ASYNC_WORKER_SCHEDULER.md)** —
  bounded asynchronous depth-priority scheduling, correctness invariants, and
  benchmark evidence.
- **[`docs/LEVEL_FORMAT_AND_CATALOG.md`](docs/LEVEL_FORMAT_AND_CATALOG.md)** —
  canonical rules and all 40 independently transcribed levels.
- **[`docs/WINDOWS_CUDA_CLIENT_PLAN.md`](docs/WINDOWS_CUDA_CLIENT_PLAN.md)** —
  staged Windows UI, remote-worker, transport, packaging, and CUDA plan.

## Repository layout

```
sketch/                     All code lives here (single flat module for now).
  BoardState.hpp             Stable 23-byte canonical board representation.
  GameRules.hpp              Shifts, reachability, ordered goals, and moves.
  LevelCatalog.hpp           Canonical connectivity-mask fixtures for 40 levels.
  ReferenceSolver.hpp        Minimum-depth exhaustive CPU oracle and replay.
  JobState.hpp               28-byte dispatcher record around compact state.
  Chain.hpp                  Intrusive singly-linked list over JobNode, O(1) splice ops.
  NodePool.hpp                Preallocated arena + free-list allocator for JobNode.
  Inbox.hpp                  Lock-free SPSC ring buffer of Chains (producer <-> dispatcher).
  Dispatcher.hpp             Round-robin batches jobs across producers, calls the CUDA kernel.
  SeedQueue.hpp               Bounded blocking master -> worker handoff queue.
  Master.hpp                 Explicit-stack DFS from the initial board down to MASTER_DEPTH.
  Worker.hpp                 Bounded asynchronous depth-priority exhaustive search.
  BeamSearch.hpp              Optional CPU-only heuristic-first pre-pass (see docs/BEAM_SEARCH.md).
  PruningHeuristics.hpp       Two board-agnostic search-space reduction heuristics.
  RemoteTransport.hpp         TCP framing and coordinator/remote seed bridges.
  SearchInstrumentation.hpp   Running observed/estimated search-size metrics.
  main_sketch.cpp             Wiring example (not compilable standalone -- board hooks are stubs).
  Makefile                   Builds and runs all tests.
  tests/                     One test file per component; see docs/BUILD_AND_TEST.md.
  sim/                       Configurable full-pipeline feasibility simulation.
docs/                        Documentation (this folder).
```

## Remaining hardware integration

The generic scheduler simulations still expose hook functions so mock and CUDA
evaluators can be substituted:

- `candidateMoves(const JobState&) -> std::vector<Move>` — legal
  (insertion point x orientation x ladybug pre-position) combinations.
- `applyMove(JobState& out, const JobState& parent, Move)` — produce the
  resulting board (pre-CUDA-evaluation).
- `allBugsEaten(const JobState&) -> bool`.
- `evaluateState(JobState&)` — CPU-side single-state reachability eval,
  only needed if you use `BeamSearch.hpp`.
- `heuristicScore(const JobState&) -> float` — only needed by `BeamSearch.hpp`.
- `launchCudaBatch(JobState*, size_t)` — your real CUDA kernel entry
  point; `Dispatcher.hpp` calls this once per batch (up to 1000 jobs).

See `docs/STATUS.md` for exactly what's tested/verified vs. still open.
