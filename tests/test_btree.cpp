// test_btree.cpp — B+-Tree unit tests and a differential test against
// std::map (the in-memory reference model at the storage layer).
//
// Small (512-byte) pages are used everywhere so splits, merges, and
// overflow chains are exercised with modest data volumes.
#include "test_util.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "btree.hpp"
#include "journal.hpp"
#include "page.hpp"
#include "pager.hpp"

using namespace sc;

namespace {

std::vector<uint8_t> payload_of(int64_t key, size_t n) {
    std::vector<uint8_t> p(n);
    for (size_t i = 0; i < n; ++i)
        p[i] = uint8_t((uint64_t(key) * 31 + i) & 0xff);
    return p;
}

// insert with a generated payload (payload_of returns a temporary, and
// BTree::insert takes a pointer)
bool ins(BTree<RowIdKey>& tree, int64_t key, size_t n) {
    std::vector<uint8_t> pl = payload_of(key, n);
    return tree.insert(RowIdKey(key), &pl) ==
           BTree<RowIdKey>::InsertResult::Inserted;
}

struct IndexKeyLess {
    bool operator()(const IndexKey& a, const IndexKey& b) const {
        return a.compare(b) < 0;
    }
};

// Every page must be accounted for: header, catalog root, tree pages, or
// freelist — and nothing twice.
void check_page_accounting(Pager& pager, const std::vector<uint32_t>& tree_pages) {
    std::set<uint32_t> all;
    for (uint32_t id = 0; id < pager.page_count(); ++id) all.insert(id);
    std::set<uint32_t> claimed{0, 1};
    for (uint32_t id : tree_pages) {
        if (id < 2)
            throw ::test::TestFailure(
                sc::str("btree claims reserved page ", id));
        if (!claimed.insert(id).second)
            throw ::test::TestFailure(sc::str("page ", id, " claimed twice by the tree"));
    }
    for (uint32_t id : freelist::collect(pager)) {
        if (!claimed.insert(id).second)
            throw ::test::TestFailure(sc::str("page ", id, " claimed twice (freelist)"));
    }
    if (claimed != all) {
        size_t orphans = 0;
        uint32_t first = 0;
        for (uint32_t id : all)
            if (!claimed.count(id)) {
                if (orphans == 0) first = id;
                ++orphans;
            }
        throw ::test::TestFailure(sc::str(orphans, " orphan page(s), e.g. page ", first,
                                          " of ", pager.page_count()));
    }
    CHECK_NO_ERROR(freelist::validate(pager));
}

} // namespace

// ---------------------------------------------------------------------
// table trees (RowIdKey)
// ---------------------------------------------------------------------

TEST(btree_table_basic_insert_search) {
    auto pager = Pager::open_memory(512);
    uint32_t root = BTree<RowIdKey>::create(*pager);
    CHECK(root >= 2);
    BTree<RowIdKey> tree(*pager, root);

    for (int64_t k = 1; k <= 60; ++k) CHECK(ins(tree, k, 40));

    for (int64_t k = 1; k <= 60; ++k) {
        std::vector<uint8_t> out;
        CHECK(tree.search(RowIdKey(k), out));
        CHECK(out == payload_of(k, 40));
    }
    std::vector<uint8_t> out;
    CHECK(!tree.search(RowIdKey(0), out));
    CHECK(!tree.search(RowIdKey(61), out));
    CHECK(!tree.search(RowIdKey(-5), out));

    auto rep = tree.validate();
    CHECK_EQ(rep.keys, uint64_t(60));
    CHECK(rep.height >= 1);
    CHECK(rep.page_ids.size() >= 2);   // 60 * ~50-byte cells don't fit one page
    CHECK(rep.warnings.empty());
    check_page_accounting(*pager, rep.page_ids);
    CHECK(!tree.render().empty());
}

TEST(btree_duplicate_rejected) {
    auto pager = Pager::open_memory(512);
    BTree<RowIdKey> tree(*pager, BTree<RowIdKey>::create(*pager));
    CHECK(ins(tree, 7, 20));
    std::vector<uint8_t> dup = payload_of(7, 999);
    CHECK(tree.insert(RowIdKey(7), &dup) ==
          BTree<RowIdKey>::InsertResult::Duplicate);
    std::vector<uint8_t> out;
    CHECK(tree.search(RowIdKey(7), out));
    CHECK_EQ(out.size(), size_t(20));   // duplicate insert changed nothing
    CHECK_EQ(tree.validate().keys, uint64_t(1));
}

TEST(btree_delete_rebalance_and_empty) {
    auto pager = Pager::open_memory(512);
    BTree<RowIdKey> tree(*pager, BTree<RowIdKey>::create(*pager));
    for (int64_t k = 0; k < 200; ++k) (void)ins(tree, k * 3 + 1, 30);
    uint32_t height_full = tree.validate().height;
    CHECK(height_full >= 2);

    // delete every other key
    for (int64_t k = 0; k < 200; ++k)
        if (k % 2 == 0) CHECK(tree.remove(RowIdKey(k * 3 + 1)));
    CHECK(!tree.remove(RowIdKey(1)));   // already gone
    auto rep = tree.validate();
    CHECK_EQ(rep.keys, uint64_t(100));
    check_page_accounting(*pager, rep.page_ids);

    // cursor sees exactly the survivors, in order
    size_t n = 0;
    int64_t prev = -1;
    auto cur = tree.cursor();
    BTree<RowIdKey>::Entry e;
    while (cur.next(e)) {
        CHECK(e.key.id > prev);
        CHECK_EQ(e.payload.size(), size_t(30));
        prev = e.key.id;
        ++n;
    }
    CHECK_EQ(n, size_t(100));

    // delete the rest: the tree collapses back to a single leaf
    for (int64_t k = 0; k < 200; ++k)
        if (k % 2 == 1) CHECK(tree.remove(RowIdKey(k * 3 + 1)));
    rep = tree.validate();
    CHECK_EQ(rep.keys, uint64_t(0));
    CHECK_EQ(rep.page_ids.size(), size_t(1));   // only the root
    CHECK_EQ(tree.height(), uint32_t(1));
    check_page_accounting(*pager, rep.page_ids);
    std::vector<uint8_t> out;
    CHECK(!tree.search(RowIdKey(4), out));
}

TEST(btree_update_payload) {
    auto pager = Pager::open_memory(512);
    BTree<RowIdKey> tree(*pager, BTree<RowIdKey>::create(*pager));
    (void)ins(tree, 1, 10);
    CHECK(tree.update_payload(RowIdKey(1), payload_of(99, 25)));
    std::vector<uint8_t> out;
    CHECK(tree.search(RowIdKey(1), out));
    CHECK(out == payload_of(99, 25));
    CHECK(!tree.update_payload(RowIdKey(2), payload_of(2, 5)));
    CHECK_EQ(tree.validate().keys, uint64_t(1));
}

TEST(btree_max_first_key) {
    auto pager = Pager::open_memory(512);
    BTree<RowIdKey> tree(*pager, BTree<RowIdKey>::create(*pager));
    RowIdKey k;
    CHECK(!tree.max_key(k));
    CHECK(!tree.first_key(k));
    int64_t keys[] = {5, -9, 100, 0, 42, -1000, 7};
    for (int64_t key : keys) (void)ins(tree, key, 8);
    CHECK(tree.max_key(k));
    CHECK_EQ(k.id, int64_t(100));
    CHECK(tree.first_key(k));
    CHECK_EQ(k.id, int64_t(-1000));
}

TEST(btree_overflow_payloads) {
    auto pager = Pager::open_memory(512);
    BTree<RowIdKey> tree(*pager, BTree<RowIdKey>::create(*pager));
    // 512-byte page: payloads above ~448 bytes spill into overflow pages
    (void)ins(tree, 1, 1500);
    (void)ins(tree, 2, 20'000);
    std::vector<uint8_t> out;
    CHECK(tree.search(RowIdKey(1), out));
    CHECK(out == payload_of(1, 1500));
    CHECK(tree.search(RowIdKey(2), out));
    CHECK(out == payload_of(2, 20'000));

    auto rep = tree.validate();   // validates the overflow chains too
    CHECK_EQ(rep.keys, uint64_t(2));
    check_page_accounting(*pager, rep.page_ids);
    uint32_t pages_with_overflow = pager->page_count();

    // removing the rows frees the overflow pages (they return to the
    // freelist rather than leaking)
    CHECK(tree.remove(RowIdKey(2)));
    CHECK(tree.remove(RowIdKey(1)));
    rep = tree.validate();
    CHECK_EQ(rep.keys, uint64_t(0));
    check_page_accounting(*pager, rep.page_ids);
    CHECK(freelist::collect(*pager).size() >= pages_with_overflow - 4);
}

TEST(btree_cursor_seek) {
    auto pager = Pager::open_memory(512);
    BTree<RowIdKey> tree(*pager, BTree<RowIdKey>::create(*pager));
    for (int64_t k = 0; k < 50; ++k) (void)ins(tree, k * 2, 5);

    // seek inclusive: first entry >= key
    {
        auto cur = tree.cursor();
        cur.seek(RowIdKey(13), true);
        BTree<RowIdKey>::Entry e;
        CHECK(cur.next(e));
        CHECK_EQ(e.key.id, int64_t(14));
    }
    // seek exclusive: first entry > key
    {
        auto cur = tree.cursor();
        cur.seek(RowIdKey(14), false);
        BTree<RowIdKey>::Entry e;
        CHECK(cur.next(e));
        CHECK_EQ(e.key.id, int64_t(16));
    }
    // before the first key: sees everything
    {
        auto cur = tree.cursor();
        cur.seek(RowIdKey(-100), true);
        BTree<RowIdKey>::Entry e;
        CHECK(cur.next(e));
        CHECK_EQ(e.key.id, int64_t(0));
    }
    // after the last key: empty
    {
        auto cur = tree.cursor();
        cur.seek(RowIdKey(1000), true);
        BTree<RowIdKey>::Entry e;
        CHECK(!cur.next(e));
    }
}

TEST(btree_root_page_is_stable) {
    auto pager = Pager::open_memory(512);
    uint32_t root = BTree<RowIdKey>::create(*pager);
    BTree<RowIdKey> tree(*pager, root);
    for (int64_t k = 0; k < 400; ++k) (void)ins(tree, k, 60);
    CHECK_EQ(tree.root(), root);
    CHECK(tree.validate().height >= 2);
    for (int64_t k = 0; k < 400; ++k) (void)tree.remove(RowIdKey(k));
    CHECK_EQ(tree.root(), root);   // splits/merges never move the root
    CHECK(!tree.render().empty());
}

TEST(btree_pathological_three_way_split) {
    // Three ~half-page cells where no 2-way partition fits: the greedy
    // multi-way fallback must split into three leaves. Cell bytes are
    // 8 (key) + 2 (length varint) + payload, so payloads of 240/241/240
    // make the pairwise sums exceed the 2-cell budget (500) while the
    // original two-cell page fits exactly.
    auto pager = Pager::open_memory(512);
    BTree<RowIdKey> tree(*pager, BTree<RowIdKey>::create(*pager));
    (void)ins(tree, 1, 240);
    (void)ins(tree, 3, 240);
    CHECK_EQ(pager->page_count(), uint32_t(3));   // header, catalog, root leaf
    (void)ins(tree, 2, 241);                      // forces the fallback

    auto rep = tree.validate();
    CHECK_EQ(rep.keys, uint64_t(3));
    CHECK_EQ(rep.height, uint32_t(2));
    check_page_accounting(*pager, rep.page_ids);

    // all three payloads intact, iteration in key order
    std::vector<uint8_t> out;
    CHECK(tree.search(RowIdKey(1), out) && out.size() == 240);
    CHECK(tree.search(RowIdKey(2), out) && out.size() == 241);
    CHECK(tree.search(RowIdKey(3), out) && out.size() == 240);
    int64_t want = 1;
    auto cur = tree.cursor();
    BTree<RowIdKey>::Entry e;
    while (cur.next(e)) CHECK_EQ(e.key.id, want++);
    CHECK_EQ(want, int64_t(4));
}

// ---------------------------------------------------------------------
// index trees (IndexKey)
// ---------------------------------------------------------------------

TEST(btree_index_ordering_and_iteration) {
    auto pager = Pager::open_memory(512);
    BTree<IndexKey> tree(*pager, BTree<IndexKey>::create(*pager));
    // structural order: NULL < INTEGER < TEXT, numerics signed, texts bytewise
    std::vector<IndexKey> keys{
        IndexKey(Value::text("abc"), 1), IndexKey(Value::null(), 1),
        IndexKey(Value::integer(5), 1), IndexKey(Value::text(""), 1),
        IndexKey(Value::integer(-3), 1), IndexKey(Value::integer(0), 1)};
    for (const auto& k : keys) (void)tree.insert(k, nullptr);
    auto rep = tree.validate();
    CHECK_EQ(rep.keys, uint64_t(6));

    std::vector<std::string> seen;
    auto cur = tree.cursor();
    BTree<IndexKey>::Entry e;
    while (cur.next(e)) seen.push_back(e.key.value.sql_literal());
    CHECK_EQ(seen.size(), size_t(6));
    CHECK_EQ(seen[0], std::string("NULL"));
    CHECK_EQ(seen[1], std::string("-3"));
    CHECK_EQ(seen[2], std::string("0"));
    CHECK_EQ(seen[3], std::string("5"));
    CHECK_EQ(seen[4], std::string("''"));
    CHECK_EQ(seen[5], std::string("'abc'"));
}

TEST(btree_index_rowid_tiebreak) {
    auto pager = Pager::open_memory(512);
    BTree<IndexKey> tree(*pager, BTree<IndexKey>::create(*pager));
    for (int64_t r = 5; r >= 1; --r)
        (void)tree.insert(IndexKey(Value::integer(42), r), nullptr);
    // same value: entries come back ordered by rowid
    auto cur = tree.cursor();
    BTree<IndexKey>::Entry e;
    int64_t expect = 1;
    while (cur.next(e)) {
        CHECK_EQ(e.key.rowid, expect);
        CHECK_EQ(e.key.value.as_int(), int64_t(42));
        ++expect;
    }
    CHECK_EQ(expect, int64_t(6));
    CHECK_EQ(tree.validate().keys, uint64_t(5));
}

TEST(btree_index_key_too_long_rejected) {
    auto pager = Pager::open_memory(512);
    BTree<IndexKey> tree(*pager, BTree<IndexKey>::create(*pager));
    // 512-byte page caps encoded index keys at 512/2 - 48 = 208 bytes
    std::string big(400, 'x');
    CHECK_DB_ERROR(tree.insert(IndexKey(Value::text(big), 1), nullptr),
                   int(Err::Constraint), "indexed value too long");
    // a small key still works afterwards
    CHECK(tree.insert(IndexKey(Value::text("ok"), 1), nullptr) ==
          BTree<IndexKey>::InsertResult::Inserted);
    CHECK_EQ(tree.validate().keys, uint64_t(1));
}

// ---------------------------------------------------------------------
// differential test vs std::map
// ---------------------------------------------------------------------

TEST(btree_differential_table_vs_std_map) {
    for (uint64_t seed : {1ull, 2ull, 3ull, 4ull}) {
        auto pager = Pager::open_memory(seed == 4ull ? 4096 : 512);
        BTree<RowIdKey> tree(*pager, BTree<RowIdKey>::create(*pager));
        std::map<int64_t, std::vector<uint8_t>> model;
        uint64_t s = seed * 6364136223846793005ull + 1442695040888963407ull;

        for (int op = 0; op < 1200; ++op) {
            int64_t key = int64_t(test::rand_below(s, 500)) - 250;
            // mostly small payloads; ~20% spill into overflow chains
            size_t plen = test::rand_below(s, 100) < 20
                              ? size_t(test::rand_below(s, 450)) + 450
                              : size_t(test::rand_below(s, 200)) + 1;
            uint64_t r = test::rand_below(s, 100);
            if (r < 50) {   // insert
                std::vector<uint8_t> pl = payload_of(key, plen);
                bool inserted = tree.insert(RowIdKey(key), &pl) ==
                                BTree<RowIdKey>::InsertResult::Inserted;
                CHECK_EQ(inserted, model.count(key) == 0);
                if (inserted) model[key] = std::move(pl);
            } else if (r < 80) {   // remove
                CHECK_EQ(tree.remove(RowIdKey(key)), model.erase(key) > 0);
            } else if (r < 85) {   // update payload
                std::vector<uint8_t> pl = payload_of(key + 7, plen);
                bool updated = tree.update_payload(RowIdKey(key), pl);
                CHECK_EQ(updated, model.count(key) > 0);
                if (updated) model[key] = std::move(pl);
            } else if (r < 90) {   // point search
                std::vector<uint8_t> out;
                bool found = tree.search(RowIdKey(key), out);
                CHECK_EQ(found, model.count(key) > 0);
                if (found) CHECK(out == model[key]);
            } else {   // range scan from the key (up to 15 entries)
                auto cur = tree.cursor();
                cur.seek(RowIdKey(key), true);
                BTree<RowIdKey>::Entry e;
                auto it = model.lower_bound(key);
                int n = 0;
                while (cur.next(e) && n < 15) {
                    CHECK(it != model.end());
                    CHECK_EQ(e.key.id, it->first);
                    CHECK(e.payload == it->second);
                    ++it;
                    ++n;
                }
                // the tree ran out exactly when the 15-entry cap or the
                // model did
                if (n == 15 && it == model.end()) CHECK(!cur.next(e));
            }
            if (op % 25 == 24) {
                auto rep = tree.validate();
                CHECK_EQ(rep.keys, uint64_t(model.size()));
                check_page_accounting(*pager, rep.page_ids);
            }
        }

        // final full-iteration comparison
        auto rep = tree.validate();
        CHECK_EQ(rep.keys, uint64_t(model.size()));
        check_page_accounting(*pager, rep.page_ids);
        CHECK(rep.height <= 6);
        auto cur = tree.cursor();
        BTree<RowIdKey>::Entry e;
        auto it = model.begin();
        size_t n = 0;
        while (cur.next(e)) {
            CHECK(it != model.end());
            CHECK_EQ(e.key.id, it->first);
            CHECK(e.payload == it->second);
            ++it;
            ++n;
        }
        CHECK_EQ(n, model.size());
        CHECK(it == model.end());
    }
}

TEST(btree_differential_index_vs_std_map) {
    for (uint64_t seed : {7ull, 8ull}) {
        auto pager = Pager::open_memory(512);
        BTree<IndexKey> tree(*pager, BTree<IndexKey>::create(*pager));
        std::map<IndexKey, int, IndexKeyLess> model;
        uint64_t s = seed * 2862933555777941757ull + 3037000493ull;

        for (int op = 0; op < 800; ++op) {
            IndexKey key;
            if (test::rand_below(s, 3) == 0)
                key.value = Value::null();
            else if (test::rand_below(s, 2) == 0)
                key.value = Value::integer(int64_t(test::rand_below(s, 40)) - 20);
            else
                key.value = Value::text("t" + std::to_string(test::rand_below(s, 30)));
            key.rowid = int64_t(test::rand_below(s, 60));

            if (test::rand_below(s, 100) < 70) {
                bool inserted =
                    tree.insert(key, nullptr) == BTree<IndexKey>::InsertResult::Inserted;
                CHECK_EQ(inserted, model.count(key) == 0);
                if (inserted) model[key] = 1;
            } else {
                CHECK_EQ(tree.remove(key), model.erase(key) > 0);
            }
            if (op % 20 == 19) {
                auto rep = tree.validate();
                CHECK_EQ(rep.keys, uint64_t(model.size()));
                check_page_accounting(*pager, rep.page_ids);
            }
        }
        auto rep = tree.validate();
        CHECK_EQ(rep.keys, uint64_t(model.size()));
        check_page_accounting(*pager, rep.page_ids);

        // iteration order must match the model exactly
        auto cur = tree.cursor();
        BTree<IndexKey>::Entry e;
        auto it = model.begin();
        size_t n = 0;
        while (cur.next(e)) {
            CHECK(it != model.end());
            CHECK(e.key.compare(it->first) == 0);
            ++it;
            ++n;
        }
        CHECK_EQ(n, model.size());
        CHECK(it == model.end());
    }
}
