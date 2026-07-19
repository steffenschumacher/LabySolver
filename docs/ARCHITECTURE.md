# Architecture: the host/CPU search & dispatch pipeline

This document explains the design in `sketch/` end to end: why it's
shaped this way, what each file does, and how they fit together. Read
`docs/OVERVIEW.md` first if you haven't.

## The core constraint this design solves

A naive exhaustive search, generating the whole move tree in memory
before evaluating it, is a non-starter: with branching ~150/move and a
7-move budget, even a **breadth-first** frontier a few levels deep
consumes tens of GB (150^4 x ~80B ≈ 400GB for 4 levels; even 3 levels is
~270MB *per concurrently-held lineage*). The whole point of this
architecture is to keep peak host memory small and roughly constant,
regardless of how deep or bushy the real search tree turns out to be,
while still keeping the GPU's batch queue full so it isn't
starved/idling between kernel launches.

The two techniques that make this work, used together:

1. **Depth-first ordering with explicit bounds**, not breadth-first. The master
   uses a strict explicit-stack DFS. Workers keep a bounded asynchronous
   frontier, always choose the deepest ready state, and enforce in-flight plus
   resident-node limits. Memory is configured per worker and does not grow with
   the full width of a level.

2. **A preallocated node arena + intrusive linked lists**, so that
   "sending 1000 candidate moves to the GPU" and "getting 1000 results
   back and re-attaching them to a search tree" are O(1) pointer-splice
   operations, never per-node copies or per-node heap
   allocation/deallocation. This is critical because the CUDA round-trip
   cadence (batches of up to 1000) happens *very* frequently relative to
   the total amount of real work.

## File-by-file

### `JobState.hpp`

The ~80-byte POD payload for one board state: raw board bytes, the move
that produced it (insertion point + orientation), and the CUDA kernel's
per-state output (`reachableMask`, `bugsEatenMask`, `offBoard`). This is
a **placeholder** — replace `boardBytes[64]` and the result fields with
your real encoding, but keep it `is_trivially_copyable` (enforced by a
`static_assert`): it lives in a preallocated arena and is never
individually constructed/destructed.

The placeholder `offBoard` flag is not sufficient for the real rules. Some
levels forbid ejecting any tile carrying the ladybug or an uneaten bug; other
levels allow the token to remain attached to the spare tile and return on a
later insertion. The real `JobState` therefore needs level-rule information
(or access to immutable level configuration) and enough state to identify all
tokens attached to the spare tile. See `docs/OVERVIEW.md` under "Tokens on the
spare tile."

### `Chain.hpp`

`JobNode` wraps one `JobState` plus `parent`/`next` pointers and
bookkeeping (`ownerId`, `level`). `Chain` is a cheap head/tail/count
descriptor over a run of `JobNode`s — not an owning container. All its
operations (`pushBack`, `append`, `takeAll`) are O(1) pointer relinking,
regardless of how many nodes are in the chain. This is what lets 1000
jobs move between a worker, the dispatcher, and back, without ever
copying a `JobState` except the one unavoidable copy into the GPU's
contiguous input buffer (see `Dispatcher.hpp` below).

`parent` pointers are also how the eventual solution path gets
reconstructed: once a winning leaf is found, walking `parent` back to
the seed root (and the seed's own stored move list — see
`SeedQueue.hpp`) recovers the whole move sequence, with zero separate
path-tracking data structure.

### `NodePool.hpp`

A preallocated arena of `JobNode`s plus a free list. `alloc()`/`release()`
are O(1) and never call `new`/`delete` after startup. `ThreadLocalPool`
wraps this with small per-producer batching so producers don't contend
on the free list's lock for every single node. Sized once at startup —
see the file's own comment for the current sizing formula
(`numProducers x maxDepth x branching`, i.e. the DFS memory bound, not
the old — and much larger — BFS-equivalent bound).

### `Inbox.hpp`

`ChainInbox<Capacity>`: a lock-free single-producer/single-consumer ring
buffer whose elements are whole `Chain` descriptors (not individual
jobs) — pushing/popping a 200-job chunk is the same O(1) cost as
pushing/popping 1 job. Used both for producer -> dispatcher submissions
and dispatcher -> producer results.

### `Dispatcher.hpp`

The single point of contact with the GPU. Each producer (master, each
worker) gets its own dedicated request inbox and result inbox — no two
producers contend on the same request queue. `Dispatcher::start()` launches a
three-stage pipeline:

- **Preprocessing thread:** round-robins across all producers' inboxes, taking at most one ~200-job
  chunk from each per round, until it's assembled a batch of up to
  `MAX_BATCH` (1000) jobs. A full batch is sent immediately; a partial batch
  is sent no later than 2ms after its first job was pulled. This round-robin is
  what prevents one producer with a huge backlog from starving everyone
  else — it can only ever contribute one chunk per round, same as a
  producer with almost nothing queued. It flattens the batch into one
  contiguous `std::vector<JobState>` — the one copy that's unavoidable because
  the GPU needs contiguous memory but the jobs actually live scattered
  across each producer's arena slice, then sends an owning batch pointer into
  a bounded three-slot queue.
- **CUDA thread:** pulls prepared batches, calls `launchCudaBatch(states, n)`
  (your real CUDA kernel), and sends the same owning pointer into a second
  bounded three-slot queue.
- **Postprocessing thread:** writes kernel output fields back into the same `JobNode`s in
  place, then splits the batch back into per-owner chains
  (O(batch size) pointer relinking, no
  payload copies) and pushes each owner's chain to its results inbox.

The bounded queues prevent preprocessing or CUDA from building an unbounded
memory backlog. `stop()` drains stages in order and must be called after all
producers have joined. Blocking `collect()` uses per-producer condition-variable
notification; `tryCollect()` stays non-blocking for opportunistic draining.

**Important gotcha, already hit and fixed once**: a producer must
interleave `submit()` calls with `tryCollect()` (non-blocking) calls
*while it's still submitting a batch*, not only after submitting
everything. If a producer's uncollected-results backlog exceeds the
results inbox's fixed capacity, postprocessing blocks trying to push more
results for that producer and eventually backpressures the bounded pipeline.
See the comment on
`tryCollect()` in the file itself.

**Second gotcha, already hit and fixed once**: never track "have I
collected everything I submitted" by counting `submit()`/`collect()`
*calls*. The dispatcher may merge several of a producer's ready chunks
into a single result chain within one prepared batch (near-guaranteed
once a producer has a backlog), so per-call counts never reliably match
up 1:1. Always track by total **node** count instead
(`results.count < nodesSubmitted`) — see `Master::tryExpand` /
`Worker::tryExpand` / `test_dispatcher_fairness.cpp` for the pattern.

### `SeedQueue.hpp`

A small bounded blocking queue that hands prefixes of the search
(computed by the master down to `MASTER_DEPTH`) off to workers as
`Seed`s (board state + the moves that produced it + its depth). Push
blocks when full (this is what naturally throttles the master to match
worker consumption, rather than the master racing ahead and building an
unbounded backlog); pop blocks when empty unless the queue has been
`finished()` (normal end-of-search) or `abort()`-ed (a solution was
found somewhere and everyone should stop waiting — see "The abort
mechanism" below).

### `Master.hpp`

Runs once, single-threaded, from the initial board. Does an
**explicit-stack depth-first search** down to `MASTER_DEPTH` (currently
4 — i.e. master owns moves 1-4). At each node reaching `MASTER_DEPTH`
alive, instead of expanding further itself, it packages the board +
reconstructed move list into a `Seed` and pushes it to the `SeedQueue`
for a worker to pick up (`emitSeed`), then releases that node
immediately.

### `Worker.hpp`

One instance per worker thread. It pulls `Seed`s from `SeedQueue` and runs a
**bounded asynchronous depth-priority exhaustive search** through the remaining
moves 5-7. It may submit several parent expansions before blocking, but always
selects the deepest evaluated-ready state and never exceeds configured
in-flight/resident limits. See `docs/ASYNC_WORKER_SCHEDULER.md` for the complete
algorithm, invariants, diagrams, and benchmarks.

#### Why bounded depth-priority, not BFS

The very first design (before this repo's current state) was
breadth-first: expand a whole level at a time, keep all of it in memory,
then expand the next level from that. Once the ladybug's own
pre/post-insertion repositioning was folded into the branching factor
(~150/move instead of ~30), that became untenable: even 3-4 levels of
BFS frontier is hundreds of MB to tens of GB *per producer* (see
`docs/OVERVIEW.md`'s branching-factor math, and `NodePool.hpp`'s sizing
comment for the exact numbers).

Strict DFS bounded memory well but exposed too little independent work to hide
CUDA latency. The current worker admits multiple expansions only within a hard
resident budget. Returned survivors enter per-depth queues; deepest states are
expanded first. Parent child-counts prove when complete negative subtrees can
be released. Thus concurrency is bounded by configuration rather than by
`branching^depth`.

**Total exhaustive work is unchanged.** Ordering and concurrency limits do not
prune. The asynchronous scheduler exists to overlap independent expansions and
keep the three-stage dispatcher full while retaining a strict memory bound.

Master retains the strict `tryExpand`/`runStack` pattern. Worker instead runs an
event loop that alternates opportunistic result classification, deepest-ready
submission, and cancellable blocking collection. Negative completion propagates
only when `outstandingChildren` reaches zero on an `expansionComplete` parent.

The current sketch still implements the older placeholder classification
`offBoard != 0 => dead`. That is correct only for levels using
forbidden-ejection mode (and only if `offBoard` means that a protected token was
ejected). In attached-token mode, a token on the spare tile is a live state when
enough moves remain to reinsert it. This classification must be replaced when
the real board state and configurable level rules are integrated.

### `SearchGlobals` (defined in `Worker.hpp`)

An atomic `solutionFound`, retained `solutionLeaf`, and mutex-protected
`publishSolution()` shared by master + all workers. Exactly one producer
publishes the pointer before a release-store makes the flag visible. Once set,
every other producer's next loop check
(`while (!solutionFound.load() && ...)`) causes it to stop looking for
more work and exit.

### The `abort()` mechanism (a deadlock fix, worth understanding)

Early testing found a real deadlock: if a worker sets `solutionFound`
while the master is concurrently blocked inside `seedQueue.push()`
(queue at capacity), and all workers immediately exit their `run()`
loops (their loop condition short-circuits `solutionFound` *before*
calling `pop()` again), then nothing is left to ever drain the
`SeedQueue` — the master can never wake from `push()`, never reaches its
own `seedQueue.finished()` call, and the whole program hangs at
`join()`.

Root cause: `SeedQueue::push()`/`pop()` only ever woke on the queue's
own `count`/`done` state — they had zero awareness of the separate
`SearchGlobals::solutionFound` flag.

**Fix**: `SeedQueue` now has an `aborted` flag and an `abort()` method
(locks, sets the flag, `notify_all()`s both condition variables); `push()`
and `pop()` treat `aborted` exactly like `done` in their wait predicates
and post-wait early-return checks. Both `Master::tryExpand` and
`Worker::tryExpand` call `abort()` on the shared `SeedQueue` at the exact
point they set `solutionFound = true`, so anyone blocked in `push()`/
`pop()` wakes immediately instead of waiting on an event that will now
never come naturally.

This is a good example of the general lesson in this codebase: **any
blocking wait that spans multiple producers must be reachable by every
condition that can make the wait pointless**, not just the "normal"
completion condition it was originally written for.

### A second bug worth knowing about (chunk-counting)

A related but distinct bug was found in `test_dispatcher_fairness.cpp`
and, once found there, confirmed to also exist (unhit so far, but
latent) in `Master.hpp`/`Worker.hpp`: `tryExpand` used to track
"have I collected all my submitted results" by counting
`submit()`/`collect()` **calls** (`chunksSubmitted`/`chunksCollected`).
But the dispatcher can merge several of a producer's ready chunks into
one result chain within a single prepared batch — so per-call counts
never reliably reach equality once a producer has any backlog, and the
final blocking drain loop (`while (chunksCollected < chunksSubmitted)`)
could hang forever. Fixed by tracking total **node** counts instead
(`nodesSubmitted` vs. `results.count`), which is invariant to how the
dispatcher batches/merges chunks. See `docs/BUILD_AND_TEST.md` /
`docs/STATUS.md` for how this was diagnosed (thread backtraces via
`lldb`) and verified (repeated runs under `timeout`).

## Data flow summary

```
                         ┌─────────────┐
                         │   Master    │  DFS, moves 1..MASTER_DEPTH
                         └──────┬──────┘
                                │ emitSeed() on reaching MASTER_DEPTH
                                v
                        ┌───────────────┐
                        │   SeedQueue    │  bounded, blocking, abort()-able
                        └───────┬────────┘
              ┌─────────────────┼─────────────────┐
              v                 v                 v
        ┌──────────┐      ┌──────────┐      ┌──────────┐
        │ Worker 1 │ ...  │ Worker k │ ...  │ Worker N │   each: DFS, moves
        └────┬─────┘      └────┬─────┘      └────┬─────┘   MASTER_DEPTH+1..7
             │                 │                 │
             └────────┬────────┴────────┬────────┘
                       v                 v
                 ┌─────────────────────────────┐
                 │         Dispatcher           │  1 request + 1 result
                 │  (1 dedicated thread)        │  inbox per producer;
                 └──────────────┬───────────────┘  round-robin batching
                                v
                       launchCudaBatch(...)          <- your CUDA kernel
```

Master and every Worker are also producers to the same `Dispatcher`
(producer ids: master = 0, workers = 1..N) — the diagram above shows the
SeedQueue and Dispatcher as conceptually separate hops, but in the
running program both connect to the *same* `Dispatcher` instance
(see `main_sketch.cpp`).
