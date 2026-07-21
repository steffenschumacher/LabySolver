# Coordinator persistence and power-loss recovery

## Recovery unit

The coordinator does not serialize `JobNode*`, mutexes, queues, or the master's
in-memory DFS stack. Those values are process-specific and unsafe to restore.
Instead, the durable recovery boundary is the self-contained depth-N `Seed`.
Deep worker/GPU work is much more expensive than deterministically replaying
the master's shallow prefix, so this gives robust recovery without pointer
swizzling or a large mutable heap image.

```mermaid
flowchart LR
    M[Master deterministic DFS] --> O[Stable seed ordinal]
    O --> C[Atomic checkpoint before enqueue]
    C --> Q[Local or remote queue]
    Q --> W[Linux/Windows GPU worker]
    W --> A[Completion with seed ID]
    A --> D[Durably remove pending seed]
```

## Snapshot contents

`CoordinatorCheckpoint.hpp` stores:

- a search fingerprint derived from the initial board and depth configuration;
- the highest deterministically generated seed ordinal;
- `masterFinished` and `solutionFound` terminal flags;
- every generated but not durably acknowledged seed, including its state and
  move prefix.

The snapshot has a magic value, schema version, payload length, and CRC-32.
Seeds use the portable protocol-v2 codec, not raw C++ structures.

## Write ordering

For each new seed:

1. Master assigns the next stable ordinal.
2. The complete pending seed is written to a sibling temporary file.
3. The file is flushed (`fsync` on Linux, `FlushFileBuffers` on Windows).
4. It atomically replaces the prior snapshot; Linux also flushes the parent
   directory.
5. Only then may the seed enter a local queue or cross the network.

For completion, the pending seed is durably removed before its metrics are
folded into live instrumentation. A crash can therefore lose a metrics sample,
but cannot incorrectly forget unfinished search work.

## Restart sequence

1. Open the checkpoint and verify framing, checksum, seed IDs, and search
   fingerprint. Corruption or a different level/configuration fails closed.
2. If `solutionFound` is set, load/report the persisted solution once solution
   value serialization is implemented; do not restart search.
3. Requeue each pending seed. They may run more than once if a pre-crash worker
   eventually returns, so completion is idempotent by stable seed ID.
4. If `masterFinished` is false, rerun its deterministic shallow DFS from the
   initial state. Ordinals at or below `generatedSeeds` are skipped because
   their pending copies were already restored or their completions were
   recorded. Generation resumes at the first new ordinal.
5. Continue ordinary local/remote distribution.

`test_master_recovery` simulates failure halfway through shallow generation and
proves restart produces exactly the missing ordinals while preserving all four
canonical seeds. `test_coordinator_checkpoint` reconstructs snapshots, tests
idempotent completion and deterministic replay, and verifies checksum and
search-identity rejection. `test_remote_end_to_end` durably tracks all 81
remote seeds and finishes with an empty pending frontier.

## Performance and remaining hardening

The current policy flushes a complete snapshot for every seed transition. It is
the strongest and simplest starting point, but can become expensive with a very
large pending frontier. Measure it before changing semantics. A later journal
may group transitions while retaining write-ahead ordering, periodic compacted
snapshots, checksummed records, and bounded recovery time.

Still required for production:

- persist a value-type solution path before setting `solutionFound`;
- add an exclusive process lock so two coordinators cannot open one checkpoint;
- define operator controls for archive/delete/start-new-search;
- authenticate and encrypt remote transport;
- add worker leases/heartbeats so disconnected seeds are promptly reassigned;
- persist sufficient instrumentation if exact continuity of estimates matters.
