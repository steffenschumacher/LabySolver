# Search-size instrumentation

Kernel throughput answers how quickly jobs are processed, but not how many jobs
the complete search is likely to contain. `sketch/SearchInstrumentation.hpp`
provides a thread-safe estimator for that second question.

## Measurements

The master and workers record one event per expanded node: the parent depth and
number of generated children. This yields observed job counts and average
branching at every depth without adding an atomic operation to every individual
job.

Every worker also accumulates the depth-5, depth-6, and depth-7 jobs belonging
to its current depth-4 seed. When the seed is exhausted, it submits one compact
sample containing those three totals. For remote workers,
`RemoteMetricsSink` sends that sample back over the existing bidirectional TCP
connection; `MetricsReceiver` merges it into the coordinator's estimator.

The current estimate is:

- depths 1--4: observed master counts when complete; while incomplete, average
  candidate and surviving-child counts from expanded parents are projected
  recursively, so pruning is reflected in the effective branching factor;
- depths 5--7: mean completed-seed subtree size at that depth multiplied by
  the number of viable depth-4 seeds emitted by the master (or its running
  estimate); and
- total jobs: sum of the seven estimated depth counts.

`snapshot()` returns observed counts, expansion counts, average branching,
estimated counts per depth, completed sample count, total estimate, and whether
the master has finished.

## Numeric range and overflow behavior

Exact observed counters remain `uint64_t` for cheap reporting, but additions
are saturating: they stop at `UINT64_MAX` and set a corresponding overflow flag
instead of wrapping to zero. The snapshot exposes these flags.

Branching factors and per-seed subtree sizes use online running means:

```text
mean += (observation - mean) / sample_count
```

They do not retain a potentially overflowing integer sum. Projected per-depth
and total sizes use `long double`, as do approximate observed totals beyond the
exact counter range. On typical x86-64 Linux this provides a maximum magnitude
around `1e4932`, vastly beyond both `uint64_t` and plausible search sizes.
Precision naturally becomes approximate at very large magnitudes, but the
instrumentation cannot silently integer-wrap.

Depth-4 jobs and emitted seeds are deliberately separate: the former includes
every candidate evaluated by the kernel, while only surviving candidates are
sent to workers. Scaling worker subtrees by all depth-4 candidates would badly
overestimate a heavily pruned search.

## Statistical limitations

This is a running estimate, not a guarantee. DFS ordering can bias early
samples if nearby subtrees have correlated sizes. Confidence improves as more
depth-4 seeds finish. Reporting the sample count alongside the estimate is
therefore essential. Randomizing seed assignment order or adding variance and
confidence-interval tracking would improve the estimator later.

After an exhaustive no-solution run, all master counts and all seed subtrees
have been observed, so the estimate converges to the actual job total. An early
solution necessarily leaves the counterfactual remainder estimated rather than
measured.

The feasibility simulator prints these figures automatically:

```sh
cd sketch
make sim
```
