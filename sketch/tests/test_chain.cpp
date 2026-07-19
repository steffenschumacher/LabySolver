// Unit tests for Chain/JobNode splicing semantics (Chain.hpp).
#include "../Chain.hpp"
#include "test_util.hpp"
#include <vector>

int main() {
    std::vector<JobNode> arena(10);

    Chain c;
    CHECK(c.empty());
    for (int i = 0; i < 5; ++i) {
        arena[i].ownerId = static_cast<uint32_t>(i);
        c.pushBack(&arena[i]);
    }
    CHECK(c.count == 5);
    CHECK(!c.empty());
    {
        int i = 0;
        for (JobNode* n = c.head; n; n = n->next, ++i)
            CHECK(n->ownerId == static_cast<uint32_t>(i));
        CHECK(i == 5);
    }
    CHECK(c.tail == &arena[4]);
    CHECK(c.tail->next == nullptr);

    // append: non-empty <- non-empty
    Chain d;
    for (int i = 5; i < 8; ++i) {
        arena[i].ownerId = static_cast<uint32_t>(i);
        d.pushBack(&arena[i]);
    }
    c.append(d);
    CHECK(c.count == 8);
    CHECK(d.empty()); // drained after append
    CHECK(d.count == 0);
    CHECK(c.tail == &arena[7]);

    // append: empty <- non-empty
    Chain e, f;
    f.pushBack(&arena[8]);
    e.append(f);
    CHECK(e.count == 1);
    CHECK(e.head == &arena[8]);
    CHECK(f.empty());

    // append: non-empty <- empty (no-op)
    Chain g, emptyChain;
    g.pushBack(&arena[9]);
    g.append(emptyChain);
    CHECK(g.count == 1);

    // takeAll drains source, transfers full contents
    Chain h = c.takeAll();
    CHECK(c.empty());
    CHECK(c.count == 0);
    CHECK(h.count == 8);
    CHECK(h.head == &arena[0]);

    // single()
    Chain s = Chain::single(&arena[0]);
    CHECK(s.count == 1);
    CHECK(s.head == &arena[0]);
    CHECK(s.tail == &arena[0]);
    CHECK(s.head->next == nullptr);

    REPORT();
}
