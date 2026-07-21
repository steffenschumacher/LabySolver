# Implementation status

## Implemented and tested

- `CompactBoardState`: stable 23-byte representation containing 36 four-bit
  connectivity masks, a ladybug position, four ordered goal positions,
  objective progress, and depth. `JobState` is now a 28-byte dispatcher record.
- Exact CPU rules: ten insertions, spare exchange, occupant movement with
  tiles, reciprocal connectivity, distinct rotations, player ejection policy,
  and consecutive ordered-goal collection.
- Canonical catalog of all 40 supplied levels without importing original
  source, assets, archives, or prose.
- Minimum-depth breadth-first reference solver, physical-state transposition
  deduplication, path reconstruction, and independent solution replay.
- Minimum solutions are proven and replayed in the automated suite for all 17
  levels whose allowance is two, three, or four pushes. The suite evaluates
  187,517 candidate states for these fixtures.
- Runtime worker maximum depth and master/worker seed depth configuration;
  fixed-size storage retains the supplied maximum of seven while each level can
  use its actual allowance. `seedDepth` is clamped to `maxDepth` by `Master`.
- Bounded asynchronous deepest-first workers with in-flight hysteresis,
  resident limits, exact sibling/subtree completion, and cancellation.
- Three-stage dispatcher with a bounded three-slot prepared queue, fair
  producer batching, immediate full-batch dispatch, and a 2 ms partial-batch
  ceiling.
- Protocol-v2 seed distribution with stable IDs, ABI-independent encoding,
  Winsock/POSIX portability, capability handshake, per-seed metrics, and
  reusable remote worker-host orchestration.
- Atomic coordinator checkpoints containing search identity, deterministic
  master progress, terminal flags, and the pending seed frontier. Recovery and
  actual remote-completion paths are tested.
- CMake/MSVC/CUDA presets and a Windows worker capability probe. Beam-search
  scaffolding, pruning helpers, and mocked-CUDA simulation remain available.

The complete suite is run with `make test` from `sketch/`.

## Important invariants

- Goals are ordered and move with their tiles. A later reachable goal cannot be
  collected before the next required one; several consecutive goals in one
  connected component are collected together.
- The ladybug may choose any currently reachable pre-insertion tile.
- `pushOut=false` rejects only moves that eject the ladybug. Goal tiles may be
  ejected, remain attached to the spare, and later return.
- A branch is pruned only after every immediate child subtree has completed
  negatively. Queue and memory pressure never imply pruning.
- Worker identity, level rules, parentage, and CUDA scratch data are not part of
  canonical board identity.
- A reported exact solution is not trusted until CPU replay succeeds. Minimum
  depth is claimed only because breadth-first search has exhausted shallower
  layers.

## Remaining work

1. Adapt the generic `Master`/`Worker` hook surface directly to
   `laby::CompactBoardState` and `laby::Move`. The exact CPU implementation is
   available, but synthetic scheduler tests still use their local hook
   implementations so concurrency can be tested independently.
2. Implement the real CUDA structure-of-arrays batch format and reachability
   kernel, then compare it byte-for-byte with the CPU oracle over generated and
   catalog-derived states.
3. Return and persist value-type solution paths from remote workers, then add
   authenticated transport, leases/heartbeats, disconnect reassignment, and
   coordinator process locking.
4. Wire real-board scoring into beam search. Progress toward the next ordered
   goal should dominate, followed by graph accessibility and remaining budget.
5. Evaluate the conservative pruning helpers against canonical child-state
   deduplication. Only enable a heuristic after exhaustive A/B tests prove that
   it preserves every shallow-level minimum solution.
6. Add a runnable CLI that selects a catalog level, exact or beam mode, local
   or remote execution, and prints/replays the result.
7. Exhaustively solve deeper catalog levels where practical and use the rest
   for beam/CUDA performance studies. A beam result proves validity after
   replay, not optimality.
8. Finish the Windows CUDA executable by adapting the real kernel/board hooks,
   then add UI and packaging in the staged order documented separately.

## Current confidence boundary

The compact representation and CPU rules are strongly covered by focused unit
tests and 17 real shallow-level minimum solutions. They are a trustworthy
oracle under the documented gameplay assumptions. The project does not yet
claim GPU equivalence, optimal solutions for deeper levels, production-ready
remote-host security, or durable solution-path persistence.
