// Verifies isUndoInsertion / oppositeEntry (Heuristic 2: skip insertions
// that would exactly undo the previous one).
#include "../PruningHeuristics.hpp"
#include "test_util.hpp"

int main() {
    // Entries 0/1 are the two ends of line 0, 2/3 of line 1, etc.
    CHECK(oppositeEntry(0) == 1);
    CHECK(oppositeEntry(1) == 0);
    CHECK(oppositeEntry(4) == 5);
    CHECK(oppositeEntry(9) == 8);
    CHECK(sameLine(0, 1));
    CHECK(!sameLine(0, 2));

    // Core case: same line, opposite end, orientation-indifferent tile,
    // ladybug didn't cross -- this is a pure undo, should be skipped.
    CHECK(isUndoInsertion(/*prevEntry=*/0, TileKind::Blank,
                          /*crossed=*/false, /*candidateEntry=*/1));
    CHECK(isUndoInsertion(0, TileKind::Cross, false, 1));

    // Ladybug DID cross to the other side during the intervening move --
    // board reverts but the ladybug's position is materially different,
    // so this is not a wasted no-op move; must not be skipped.
    CHECK(!isUndoInsertion(0, TileKind::Blank, /*crossed=*/true, 1));
    CHECK(!isUndoInsertion(0, TileKind::Cross, true, 1));

    // Straight/Corner/Stub tiles have >1 distinct orientation, so
    // "any orientation reinserts identically" doesn't hold -- this
    // heuristic deliberately stays conservative and does not flag these.
    CHECK(!isUndoInsertion(0, TileKind::Straight, false, 1));
    CHECK(!isUndoInsertion(0, TileKind::Corner, false, 1));
    CHECK(!isUndoInsertion(0, TileKind::Stub, false, 1));

    // Candidate entry is the SAME end again (not the opposite end) --
    // that's a different insertion (pushes further in the same
    // direction), not an undo of the previous one.
    CHECK(!isUndoInsertion(0, TileKind::Blank, false, 0));

    // Candidate entry belongs to a different line entirely -- never an
    // undo of a different line's insertion, regardless of tile kind.
    CHECK(!isUndoInsertion(0, TileKind::Blank, false, 2));
    CHECK(!isUndoInsertion(0, TileKind::Blank, false, 5));

    // Sanity sweep: for every (prevEntry, tileKind, crossed) combination,
    // exactly one candidateEntry (the opposite end) should ever be
    // flagged, and only when indifferent-orientation + not-crossed.
    for (EntryPoint prev = 0; prev < NUM_ENTRIES; ++prev) {
        for (TileKind k : {TileKind::Blank, TileKind::Cross, TileKind::Straight, TileKind::Corner,
                           TileKind::Stub}) {
            for (bool crossed : {false, true}) {
                int flaggedCount = 0;
                for (EntryPoint cand = 0; cand < NUM_ENTRIES; ++cand) {
                    if (isUndoInsertion(prev, k, crossed, cand)) {
                        ++flaggedCount;
                        CHECK(cand == oppositeEntry(prev));
                    }
                }
                bool shouldFlagOne = isOrientationIndifferent(k) && !crossed;
                CHECK(flaggedCount == (shouldFlagOne ? 1 : 0));
            }
        }
    }

    REPORT();
}
