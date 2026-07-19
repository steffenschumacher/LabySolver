// Verifies reducePrePositions (Heuristic 1: ladybug pre-position
// deduplication) against a small hand-built graph, including a "trap"
// case designed to catch an unsound implementation that merges two
// off-line cells whose only real connection routes through an on-line
// cell (which the insertion is about to change).
#include "../PruningHeuristics.hpp"
#include "test_util.hpp"

#include <map>
#include <set>
#include <cstdio>

using Cell = int;

// A tiny hand-built graph (not a real board -- just enough structure to
// exercise the component logic):
//
//   0 - 1 - 2      3 - 4        (component A: {0,1,2}, component B: {3,4})
//           |
//          10 (on affected line)
//           |
//           5 - 6                (component C: {5,6}, only reachable from
//                                  A/B via the on-line cell 10)
//
// Cells 10, 11 are "on the affected line". 11 is isolated (reachable, on
// the line, no other reachable neighbors) -- kept individually regardless.
static const std::map<Cell, std::vector<Cell>> kAdjacency = {
    {0, {1}},     {1, {0, 2}}, {2, {1, 10}},
    {3, {4}},     {4, {3}},    {10, {2, 5}}, // on-line cell bridges component {0,1,2} and {5,6}
    {5, {10, 6}}, {6, {5}},    {11, {}},     // on-line, isolated
};

static std::vector<Cell> neighborsOf(Cell c) {
    auto it = kAdjacency.find(c);
    return it == kAdjacency.end() ? std::vector<Cell>{} : it->second;
}

static bool onLine(Cell c) {
    return c == 10 || c == 11;
}

int main() {
    // Case 1: reachable set spans two genuinely separate off-line
    // components ({0,1,2} and {3,4}) plus nothing on the line. Expect
    // exactly 2 representatives back (one per component).
    {
        std::vector<Cell> reachable = {0, 1, 2, 3, 4};
        auto reduced = reducePrePositions(reachable, neighborsOf, onLine);
        CHECK(reduced.size() == 2);

        std::set<Cell> reducedSet(reduced.begin(), reduced.end());
        bool oneFromEach = (reducedSet.count(0) || reducedSet.count(1) || reducedSet.count(2)) &&
                           (reducedSet.count(3) || reducedSet.count(4));
        CHECK(oneFromEach);
        // Exactly one rep per component, not one rep total or one per cell.
        int fromABC = reducedSet.count(0) + reducedSet.count(1) + reducedSet.count(2);
        int fromD = reducedSet.count(3) + reducedSet.count(4);
        CHECK(fromABC == 1);
        CHECK(fromD == 1);

        std::printf("  case1: reachable=5 cells -> %zu representative(s)\n", reduced.size());
    }

    // Case 2 (the trap): reachable set includes {0,1,2} AND {5,6}, which
    // are ONLY connected to each other via cell 10 -- which is ON the
    // affected line. A correct implementation must NOT merge {0,1,2} with
    // {5,6} into one component (their post-insertion equivalence isn't
    // guaranteed, since cell 10 itself is about to change). Expect 2
    // representatives (one for {0,1,2}, one for {5,6}), not 1.
    {
        std::vector<Cell> reachable = {0, 1, 2, 5, 6}; // note: 10 itself NOT reachable here
        auto reduced = reducePrePositions(reachable, neighborsOf, onLine);
        CHECK(reduced.size() == 2);
        std::printf("  case2 (trap): reachable={0,1,2,5,6} (bridge cell 10 not reachable) -> %zu "
                    "representative(s) (must be 2, not 1)\n",
                    reduced.size());
    }

    // Case 3: the bridge cell 10 IS reachable (on the line) -- must be
    // kept individually (not merged into either side), in addition to one
    // representative per off-line component on each side.
    {
        std::vector<Cell> reachable = {0, 1, 2, 10, 5, 6};
        auto reduced = reducePrePositions(reachable, neighborsOf, onLine);
        std::set<Cell> reducedSet(reduced.begin(), reduced.end());
        CHECK(reducedSet.count(10) == 1); // on-line cell always kept
        CHECK(reduced.size() == 3);       // {0,1,2} rep + 10 + {5,6} rep
        std::printf("  case3: reachable includes bridge cell 10 -> %zu representative(s) (2 "
                    "off-line reps + 1 on-line kept individually)\n",
                    reduced.size());
    }

    // Case 4: an isolated on-line cell (11) mixed with an off-line
    // component -- 11 must be kept regardless of having no neighbors.
    {
        std::vector<Cell> reachable = {0, 1, 2, 11};
        auto reduced = reducePrePositions(reachable, neighborsOf, onLine);
        std::set<Cell> reducedSet(reduced.begin(), reduced.end());
        CHECK(reducedSet.count(11) == 1);
        CHECK(reduced.size() == 2); // {0,1,2} rep + 11
    }

    // Case 5: empty reachable set -> empty result (no crash).
    {
        std::vector<Cell> reachable = {};
        auto reduced = reducePrePositions(reachable, neighborsOf, onLine);
        CHECK(reduced.empty());
    }

    // Case 6: every reachable cell is alone in its own component (no
    // edges between any of them at all) -- should return all of them
    // unchanged (each is its own representative).
    {
        std::vector<Cell> reachable = {3, 4}; // 3-4 ARE connected, use disjoint instead
        // Use two mutually-unreachable-by-graph cells instead: 4 and 6
        // aren't adjacent to each other in kAdjacency at all.
        std::vector<Cell> disjoint = {4, 6};
        auto reduced = reducePrePositions(disjoint, neighborsOf, onLine);
        CHECK(reduced.size() == 2);
    }

    REPORT();
}
