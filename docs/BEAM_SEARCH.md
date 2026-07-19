# Beam Search: what it is and what it's for

`BeamSearch.hpp` is a completely separate, optional, **CPU-only**,
**non-exhaustive** search — a fast heuristic shortcut you can try before
paying for the full brute-force pipeline (`Master`/`Worker`/`Dispatcher`).
It has nothing to do with the CUDA kernel, the node pool, chains, or any
of the memory-bounding machinery described in `docs/ARCHITECTURE.md` — it's
a small, self-contained algorithm that just runs on one CPU thread.

## The problem it's trying to avoid

The full brute-force search is *exhaustive*: it is guaranteed to find a
solution if one exists within the move budget, because it (eventually)
tries every legal sequence of moves. That guarantee is valuable, but it
costs a lot of work — potentially billions of board states — even though,
in practice, most solvable levels probably have an "obvious-ish" solution
that a much smaller, smarter search would stumble onto quickly.

Beam search is a classic technique for exactly this trade-off: give up
the *guarantee* of finding a solution, in exchange for often finding one
using orders of magnitude less work. Think of it as "the brute-force
tree, but at every level we throw away all but the most promising-looking
branches, instead of keeping all of them."

## The core idea, in plain language

Imagine you're 1 move into the puzzle. Instead of trying literally every
possible next move (which is what the brute-force search does), you:

1. **Generate all of them anyway** (there's usually only ~30-150 at this
   point, cheap).
2. **Score each resulting board** with a heuristic — a quick, rough
   guess at "how good is this position?" (e.g. "how many bugs have been
   eaten, and how far is the ladybug from the nearest remaining bug?").
   Lower score = better/more promising, by this codebase's convention.
3. **Keep only the best N** (`N` = the "beam width", e.g. 64) and
   throw the rest away completely — they're gone, never revisited.
4. Repeat from step 1, but only expanding moves from those N survivors,
   for as many levels as your move budget allows (e.g. 7).
5. If at any point one of the surviving boards has all 4 bugs eaten,
   you're done — that's your solution.

The name "beam" comes from imagining a flashlight beam sweeping forward
through the tree, only illuminating (keeping) the most promising `N`
paths at each step, rather than the whole tree.

### Why this is much cheaper than the full brute force

Total work is roughly `beamWidth x branchingFactor x depth`, e.g.
`64 x 30 x 7` ≈ 13,000 board evaluations for the *entire* search — versus
potentially billions for the exhaustive approach. That's small enough to
run single-threaded, synchronously, on the CPU, with plain
`std::vector`s — no need for the node pool, chains, dispatcher, or GPU
batching at all. It typically finishes in a small fraction of a second.

### The catch: it can fail even when a solution exists

Because you're throwing away everything except the top `N` candidates
at every level, it's entirely possible the *actual* solution path
requires, at some point, taking a move that *looked* worse by your
heuristic than the alternatives you kept — and once you've discarded it,
it's gone forever. This is called getting stuck behind a **local
optimum**: everything reachable from your current surviving beam looks
worse than where you already are, even though a genuinely better path
existed a few moves back, disguised as a worse-looking option.

Pure greedy search (beam width = 1: always just take the single
best-looking move) is the most extreme, most failure-prone version of
this. This codebase's own test (`tests/test_beam_search.cpp`) builds a
deliberately adversarial synthetic board with a planted "decoy path"
specifically to demonstrate this failure mode — with beam width 1, the
search reliably fails, confirming the risk is real, not just
theoretical.

## How this codebase mitigates that risk (two techniques)

### 1. Diversity injection (a light touch of genetic-algorithm style exploration)

Instead of keeping *only* the top `N` by score, `selectSurvivors()`
keeps most of the beam as the genuine best-scoring candidates ("elites"),
but reserves a small fraction of the beam slots (controlled by
`BeamSearchConfig::diversityFraction4` — e.g. `1` means roughly 1/(1+1) =
half... actually 1/(fraction+1) of the beam, so `diversityFraction4 = 1`
reserves 1/2, `= 3` reserves 1/4, etc. — see the field's own comment) for
candidates picked **at random** from everyone who *didn't* make the elite
cut. This means a board that scored poorly by the heuristic — but might
actually be on the real solution path — still has a chance of surviving
into the next level, rather than being deterministically discarded every
single time.

### 2. Multiple restarts

Because the diversity injection above uses randomness, running the whole
beam search more than once (`BeamSearchConfig::restarts`, e.g. 8) with a
different random seed/shuffle each time gives multiple independent
chances to get lucky and keep the one "looks bad but is actually right"
branch that a single run might have discarded. `trySolveHeuristicFirst()`
just loops over `restarts` attempts and returns as soon as any one of
them succeeds.

### Test evidence

`tests/test_beam_search.cpp` builds a small synthetic board (not the real
game — a simplified graph model just for testing this algorithm in
isolation) with a deliberately planted trap: a heuristically-attractive
"decoy" path that looks better step-by-step but is actually a dead end,
versus a real solution path that looks temporarily worse. Results:

- **Greedy-only** (beam width 1, no diversity): fails to solve it, as
  expected/designed.
- **Diversified beam search** (width 16, ~1/4 of the beam kept random,
  20 restarts): finds the planted solution, having explored only
  **0.19%** of the full state space that an exhaustive search would have
  needed to cover.

This is the concrete demonstration that the technique works as intended
on at least one adversarial case; it is not a guarantee about the real
game's board, which has not yet been tested against this algorithm (see
`docs/STATUS.md`).

## What you need to plug in to use it

Two board-specific hook functions, declared but not defined in
`BeamSearch.hpp` (same pattern as `Worker.hpp`'s `candidateMoves`/
`applyMove`/`allBugsEaten`):

- **`evaluateState(JobState&)`** — fills in the reachability/bugs-eaten
  fields for a *single* board state, synchronously, on the CPU. This is
  the same computation the CUDA kernel does per job, just for one state
  at a time instead of a batch of up to 1000. Either port your kernel's
  logic to a plain CPU function, or (if that's awkward) just call
  `launchCudaBatch(&state, 1)` — beam search's volumes are so low
  (~13k total evaluations) that a CPU port is almost certainly faster in
  practice than round-tripping to the GPU at that scale, but either
  works correctness-wise.
- **`heuristicScore(const JobState&) -> float`** — your "how good is
  this board?" function. Lower = better, by convention in this code.
  A reasonable starting point: heavily reward more bugs eaten (dominant
  term), then rank remaining boards by *reachable-graph distance*
  (number of hops through actually-connected tiles, not straight-line/
  Manhattan distance, since walls matter a lot in this game) to the
  nearest still-uneaten bug. Tuning this well is likely to matter a lot
  for how often beam search actually succeeds — it has not yet been
  tuned against the real game in this repository.

## How it's meant to be used (recommended, not yet wired up)

```cpp
std::vector<Move> path;
if (trySolveHeuristicFirst(startState, BeamSearchConfig{}, path)) {
    // Done — solution found cheaply, no GPU/brute-force needed.
} else {
    // Beam search failed to find a solution (doesn't mean none exists!)
    // — fall back to the full Master/Worker/Dispatcher brute-force
    // pipeline, which is exhaustive within the move budget.
}
```

This composition (try beam search first, fall back to brute force) is
**not yet implemented** in `main_sketch.cpp` — it's currently a
standalone module with its own test. See `docs/STATUS.md` for this and
other open items.
