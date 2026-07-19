# CPU pipeline feasibility simulation

`sketch/sim/feasibility_sim.cpp` runs the real host-side `Master`, `Worker`,
`Dispatcher`, `NodePool`, and `SeedQueue` against a deterministic synthetic
search tree. It does not simulate the maze rules. Its purpose is to measure and
stress the concurrency architecture before the real board representation and
CUDA kernel are available.

The fake CUDA function classifies a configurable fraction of candidates as
survivors and optionally waits for a configured time on every launch. The
search deliberately contains no solution, so it must exhaust all surviving
branches and exercise normal queue completion and node cleanup. A fixed seed
makes a given configuration reproducible.

## Build and run

From `sketch/`:

```sh
make sim
```

The default approximates the expected host:

```sh
./bin/feasibility_sim \
  --workers 10 \
  --branching 150 \
  --survival 0.03 \
  --kernel-us 100 \
  --seed 1
```

Options:

- `--workers N`: number of worker threads; the master and dispatcher each use
  an additional thread.
- `--branching N`: candidates emitted by every expansion. The expected real
  workload is roughly 150 after including ladybug pre-position choices.
- `--survival RATE`: fraction of synthetic kernel results allowed to continue
  deeper. Runtime grows rapidly with this value because retained branching is
  approximately `branching * survival`.
- `--kernel-us N`: fake latency per CUDA launch. This models round-trip delay,
  not actual GPU compute or PCIe transfer behavior.
- `--seed N`: deterministic workload variation.
- `--inflight-high N` / `--inflight-low N`: asynchronous worker hysteresis.
- `--resident N`: strict resident-node budget per worker.
- `--progress-ms N`: live instrumentation interval; defaults to one second and
  may be disabled with zero.
- `--max-seconds N`: stop a sustained feasibility run after this many seconds;
  zero means exhaust the synthetic tree.

The report includes wall and process CPU time, average CPU cores occupied,
jobs/second, batch-fill statistics, pipeline full/deadline flush counts, and
peak/final node-pool use. A successful
run must finish with zero nodes in use.

For a one-minute mock using the assumption that a full 1,000-job CUDA batch
takes 500 microseconds (an ideal ceiling of 2 million jobs/second):

```sh
./bin/feasibility_sim \
  --workers 10 --branching 150 --survival 0.06 \
  --kernel-us 500 --progress-ms 1000 --max-seconds 60
```

The time-limited mode uses the existing global early-stop path. DFS ancestors
retained for possible solution reconstruction are intentionally not unwound;
the simulator reports them and the process releases the arena on exit. Normal
exhaustive runs still require final node-pool use to be zero.

## Interpreting results

This measures whether the CPU pipeline can generate, dispatch, return, and
recycle jobs at the required rate. It does **not** predict solver completion
time until real pruning/survival rates and real CUDA timings are known. Useful
experiments are:

```sh
# CPU-side upper bound: fake kernel returns immediately
./bin/feasibility_sim --kernel-us 0

# Higher launch latency / backpressure
./bin/feasibility_sim --kernel-us 1000

# Repeat deterministic variants to expose timing-sensitive failures
for seed in 1 2 3 4 5; do
  timeout 60 ./bin/feasibility_sim --seed "$seed" || break
done
```

Keep `branching * survival` modest for an exhaustive depth-7 run. For example,
the default retained branching is about 4.5. Raising survival from `0.03` to
`0.10` changes retained branching to about 15 and can increase total work by
orders of magnitude.
