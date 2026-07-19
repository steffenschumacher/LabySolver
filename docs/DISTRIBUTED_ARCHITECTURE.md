# Distributed multi-GPU architecture

## Partition at the seed boundary

The coordinator runs the existing `Master` through `MASTER_DEPTH` (currently
four moves). Each surviving depth-4 state is already represented by a
self-contained `Seed`: board state, depth, and the moves that produced it.
Those seeds are the unit of network distribution.

Each remote GPU host runs its own:

- bounded incoming `SeedQueue`;
- CPU worker threads and thread-local pool caches;
- host-local `NodePool`;
- `Dispatcher`; and
- CUDA context/kernel.

Jobs below a seed never cross the network. A remote worker expands moves 5--7
locally and batches them directly to that host's GPU. This is preferable to
remote per-job dispatch because a seed is small, represents an independent
subtree, and can produce thousands or millions of GPU jobs without further
network traffic.

```text
                         TCP seed streams
Coordinator Master  +--------------------------> Host A SeedQueue
  depth 0..4        |                              Workers -> Dispatcher -> GPU A
  NodePool          +--------------------------> Host B SeedQueue
                                                   Workers -> Dispatcher -> GPU B
```

## Implementation

`sketch/RemoteTransport.hpp` provides:

- a versioned, length-delimited binary protocol;
- reliable partial read/write loops;
- TCP connect/listen/accept helpers;
- `Seed` serialization;
- a concurrency-weighted `SeedDistributor` on the coordinator;
- a `SeedReceiver` that bridges a connection into a remote host's ordinary
  `SeedQueue`;
- `Finished`, `Abort`, and `SolutionFound` control message types; and
- natural backpressure: a full remote queue stops its receiver, which fills
  the TCP send buffer and eventually blocks the coordinator distributor.

Each peer advertises its local worker-thread count to `SeedDistributor`. The
weighted schedule initially sends approximately one seed per available worker
slot, then continues proportional prefetch. Thus a 10-thread host receives ten
assignments per scheduling cycle while a 4-thread host receives four. The
remote bounded queue can hold additional seeds so workers do not wait on a
network round trip between subtrees.

Remote instrumentation uses the connection in the reverse direction. A worker
sends one `SeedMetrics` message after finishing a seed; it does not report every
job. `MetricsReceiver` merges these samples at the coordinator. See
`docs/INSTRUMENTATION.md`.

The protocol's maximum payload is 64 KiB. Version 1 sends `JobState` as raw
bytes, so all hosts must run the same solver build and ABI. This is suitable
for homogeneous Ubuntu/x86-64 GPU machines. A stable field-by-field board
encoding should replace it if heterogeneous architectures or rolling upgrades
are needed.

## Lifecycle

1. Start one remote service per GPU host. It listens for a coordinator, creates
   its local pool/dispatcher/workers, then runs `SeedReceiver`.
2. The coordinator connects with `connectTcp`, wraps each descriptor in a
   `FramedSocket`, and starts `SeedDistributor`.
3. `Master` fills its local bounded seed queue. The distributor assigns seeds
   round-robin; TCP and both bounded queues provide backpressure.
4. When the master exhausts its tree, its queue becomes finished. The
   distributor broadcasts `Finished`; remote receivers finish their local
   queues after all preceding seeds have arrived, and workers drain normally.
5. For early termination, `Abort` causes each remote receiver to abort its
   local queue. `SolutionFound` is reserved for the reverse notification from
   a remote host.

## Current scope and next integration point

Seed distribution, completion, abort reception, socket failure handling, and
multi-host exhaustive execution are implemented and tested. Automatic
cross-host solution propagation is not yet wired into `Worker`: the current
`SearchGlobals` exposes a process-local `JobNode*`, which cannot be sent over a
network and does not retain the master's seed prefix in a directly exportable
result object. Before real solving, solution reconstruction should be changed
to produce a value-type `Solution` containing all moves. A remote supervisor
can then serialize that value in `SolutionFound`, while the coordinator aborts
its master queue and broadcasts `Abort` to every other host.

Other production concerns deliberately left outside this first implementation:

- authentication/encryption (use a trusted private network or TLS tunnel);
- reconnecting and reissuing seeds after a host failure;
- dynamic load balancing based on host speed rather than round-robin count;
- durable checkpointing; and
- heartbeat/time-out policy.

These do not affect the seed-level partitioning, but fault-tolerant operation
requires seed IDs and coordinator-side tracking of assigned versus completed
seeds.

## Tests

- `test_remote_transport`: codec fidelity, two-peer round-robin delivery,
  finish handling, and abort handling over stream sockets.
- `test_remote_end_to_end`: one coordinator plus two simulated remote GPU
  hosts, each with independent workers, dispatcher, fake GPU, seed queue, and
  node pool. It exhausts the complete synthetic depth-7 tree and verifies exact
  seed/job counts and zero pool leaks.

The tests use Unix stream socket pairs to avoid depending on machine network
configuration; they exercise the same framing and partial I/O code used by TCP.
