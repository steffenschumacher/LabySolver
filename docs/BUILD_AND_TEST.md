# Building and testing

## Prerequisites

- A C++17 compiler with `<thread>`/`<atomic>`/`<condition_variable>`
  support (`clang++` or `g++`; developed against Apple clang, should work
  unmodified on Linux).
- No external dependencies — everything is header-only, plus one `.cpp`
  test file per component.
- The real CUDA kernel (`launchCudaBatch`) is **not** part of this repo's
  build; all tests either stub it out with a trivial fake (see
  `tests/test_dispatcher_fairness.cpp`, `tests/test_end_to_end_sim.cpp`)
  or don't need it at all (`test_chain`, `test_nodepool`, `test_inbox`,
  `test_seedqueue`, `test_pruning_*`).

## Building and running all tests

From `sketch/`:

```sh
make test
```

This builds every file listed in the Makefile's `TESTS` variable and
runs each one in turn, stopping at the first failure (`exit 1` on any
non-zero exit code). Each test prints a `N/N checks passed` summary line
on success.

To just build without running:

```sh
make            # or: make all
```

To build/run one specific test:

```sh
make bin/test_pruning_undo_move
./bin/test_pruning_undo_move
```

To clean build artifacts:

```sh
make clean
```

## Running the feasibility simulation

The test suite uses small deterministic workloads for correctness. To stress
and measure the complete CPU pipeline with a configurable fake CUDA call, run:

```sh
make sim
```

See `docs/SIMULATION.md` for workload controls and guidance on interpreting the
reported throughput, batch fill, CPU use, and node-pool peak.

### macOS-specific note

There's a documented workaround in the `Makefile` for a broken standalone
Xcode Command Line Tools install (`-isystem
/Library/Developer/CommandLineTools/SDKs/MacOSX26.4.sdk/usr/include/c++/v1`).
This is harmless on a normal setup and a no-op on Linux/CUDA build
machines — remove that line there if it causes any issue, it's only
needed for this specific broken local environment.

## What each test covers

| Test | What it verifies |
|---|---|
| `test_chain` | `Chain`'s O(1) splice operations (`pushBack`, `append`, `takeAll`) preserve order and counts correctly. |
| `test_nodepool` | `NodePool`/`ThreadLocalPool` alloc/release under stress; no leaks, peak in-use tracked correctly. |
| `test_inbox` | `ChainInbox` SPSC ring buffer: FIFO order preserved across many transfers. |
| `test_seedqueue` | `SeedQueue` push/pop under concurrent producer/consumer load: every seed delivered exactly once, no gaps or duplicates. |
| `test_beam_search` | `BeamSearch.hpp` against a synthetic adversarial "decoy path" board: confirms pure greedy (beam width 1) fails as expected, and diversified beam search (width 16, restarts) finds the planted solution while exploring a tiny fraction of the state space. See `docs/BEAM_SEARCH.md`. |
| `test_pruning_undo_move` | `isUndoInsertion`/`oppositeEntry`/`sameLine`: exhaustive sweep over every `(entry, tileKind, crossed)` combination. See `docs/PRUNING_HEURISTICS.md`. |
| `test_pruning_reduce_positions` | `reducePrePositions` against a hand-built graph, including the deliberate "bridge-through-an-on-line-cell" trap case. See `docs/PRUNING_HEURISTICS.md`. |
| `test_dispatcher_pipeline` | Three-stage preprocessing/CUDA/postprocessing handoff, 2ms partial-batch deadline, immediate full batches, result write-back, hard batch limit, and clean drain. |
| `test_dispatcher_fairness` | `Dispatcher`'s round-robin batching: a producer with a huge backlog must not starve producers with small backlogs. Also exercises the submit/collect interleaving and total-node-count completion tracking (see `docs/ARCHITECTURE.md`'s "gotchas"). |
| `test_worker_async` | Exact exhaustive job sets, duplicate/loss detection, deepest-first ordering, hysteresis, completion propagation, and strict memory bounds on regular and tight-memory trees. |
| `test_worker_cancellation` | Ten repeated mid-flight global cancellations with multiple workers, bounded pipeline drain, producer abandonment, and no false cleanup requirement for untested nodes. |
| `test_end_to_end_sim` | The full `Master` + asynchronous `Worker` + three-stage `Dispatcher` + `SeedQueue` pipeline against a planted solution. Verifies the reconstructed path, bounded arena use, and expected early-solution retention behavior. |
| `test_search_instrumentation` | Per-depth counts, branching averages, completed-seed sampling, and total-tree estimation against an exactly known regular tree. |
| `test_remote_transport` | Versioned seed codec/framing, round-robin delivery to two remote queues, finish propagation, and abort propagation over stream sockets. |
| `test_remote_end_to_end` | A coordinator master and two independent simulated remote GPU hosts exhaust a synthetic depth-7 tree, verifying weighted seed assignment, metrics return, exact size estimation, job counts, and pool cleanup. |

## A note on flaky/hang-prone tests

Two real concurrency bugs were found and fixed in this codebase by
running these tests **repeatedly** (not just once) — a single passing
run does not prove a concurrency bug is absent, since both bugs were
timing-dependent (they only manifested when a solution was found at a
specific moment relative to another thread's blocking call, or when a
producer's backlog crossed a specific threshold). If you touch
`Master.hpp`, `Worker.hpp`, `SeedQueue.hpp`, or `Dispatcher.hpp`, get in
the habit of running the affected test several times in a row under a
timeout before trusting a single green run, e.g.:

```sh
for i in $(seq 1 10); do
  timeout 15 ./bin/test_end_to_end_sim || echo "run $i FAILED/HUNG"
done
```

If a test hangs, useful diagnosis techniques (both used to find the two
bugs described in `docs/ARCHITECTURE.md`):

```sh
# Confirm busy-spin vs. genuinely blocked/asleep:
ps -o pid,%cpu,stat,command -p <pid>

# Full thread-by-thread backtrace without needing a special debug build:
lldb -p <pid> -o "thread backtrace all" -o "detach" -o "quit"
```

(Note: this environment's shell rules require killing hung processes
with an explicit literal numeric PID, e.g. `kill -9 1234` — not a
variable-substitution pattern like `kill $PID` in the same invocation.)
