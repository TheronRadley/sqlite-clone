// test_pager.cpp — pager, cache, freelist, and file-header tests
// (storage-layer tests that never touch SQL).
#include "test_util.hpp"

#include <cstdio>
#include <set>

#include "journal.hpp"
#include "page.hpp"
#include "pager.hpp"

using namespace sc;

TEST(pager_create_and_roundtrip) {
    std::string path = test::temp_db_path();
    {
        auto pager = Pager::open_file(path);
        CHECK(pager->is_file());
        CHECK_EQ(pager->page_size(), uint32_t(4096));
        CHECK_EQ(pager->page_count(), uint32_t(2));   // header + catalog root
        CHECK(pager->page_exists(1));
        CHECK(!pager->page_exists(2));

        pager->begin_txn();
        uint32_t a = pager->allocate_page();
        uint32_t b = pager->allocate_page();
        CHECK_EQ(a, uint32_t(2));
        CHECK_EQ(b, uint32_t(3));
        CHECK_EQ(pager->page_count(), uint32_t(4));

        std::vector<uint8_t> pa(4096, 0);
        pa[10] = 'h';
        pager->write_page(a, pa);
        pager->commit_txn();
    }
    {
        auto pager = Pager::open_file(path);
        CHECK_EQ(pager->page_count(), uint32_t(4));
        auto back = pager->get_page(2);
        CHECK_EQ(back[10], uint8_t('h'));
        // page 1 is the catalog root the pager pre-creates
        CHECK(page_type(Page(pager->get_page(1))) == PageType::TableLeaf);
    }
    std::remove(path.c_str());
}

TEST(pager_invalid_page_ids_rejected) {
    auto pager = Pager::open_memory();
    CHECK(!pager->is_file());
    CHECK_DB_ERROR(pager->get_page(2), int(Err::Pager), "out of range");
    CHECK_DB_ERROR(pager->get_page(999), int(Err::Pager), "out of range");
    CHECK_DB_ERROR(pager->write_page(5, std::vector<uint8_t>(4096, 0)),
                   int(Err::Pager), "out of range");
    CHECK_DB_ERROR(pager->write_page(0, std::vector<uint8_t>(17, 0)),
                   int(Err::Pager), "bytes");
}

TEST(pager_dirty_tracking_and_flush) {
    auto pager = Pager::open_memory();
    pager->set_cache_capacity(64);
    pager->begin_txn();
    uint32_t a = pager->allocate_page();
    std::vector<uint8_t> p(4096, 1);
    pager->write_page(a, p);
    CHECK(pager->is_dirty(a));
    CHECK_EQ(pager->dirty_pages(), size_t(1));
    pager->commit_txn();
    CHECK(!pager->is_dirty(a));
    CHECK_EQ(pager->dirty_pages(), size_t(0));
    // committed image is readable back
    CHECK_EQ(pager->get_page(a)[0], uint8_t(1));
}

TEST(pager_cache_eviction_stats) {
    auto pager = Pager::open_memory();
    pager->set_cache_capacity(8);
    pager->begin_txn();
    for (int i = 0; i < 40; ++i) {
        uint32_t id = pager->allocate_page();
        std::vector<uint8_t> p(4096, uint8_t(i));
        pager->write_page(id, p);
    }
    pager->commit_txn();
    CHECK_EQ(pager->page_count(), uint32_t(42));

    PagerStats before = pager->stats();
    // hot set of 6 pages: first round misses, later rounds hit
    for (int round = 0; round < 5; ++round)
        for (uint32_t id = 2; id < 8; ++id) (void)pager->get_page(id);
    CHECK(pager->stats().cache_misses > before.cache_misses);
    CHECK(pager->stats().cache_hits > before.cache_hits);
    CHECK(pager->stats().evictions >= before.evictions);
    CHECK(pager->cached_pages() <= pager->cache_capacity());
    CHECK_NO_ERROR(freelist::validate(*pager));
}

TEST(pager_freelist_alloc_free_cycles) {
    auto pager = Pager::open_memory();
    std::vector<uint32_t> ids;
    pager->begin_txn();
    for (int i = 0; i < 30; ++i) ids.push_back(pager->allocate_page());
    pager->commit_txn();
    uint32_t high_water = pager->page_count();
    CHECK_EQ(high_water, uint32_t(32));

    pager->begin_txn();
    for (size_t i = 0; i < 20; ++i) pager->free_page(ids[i]);
    pager->commit_txn();
    CHECK_NO_ERROR(freelist::validate(*pager));
    auto free_pages = freelist::collect(*pager);
    CHECK_EQ(free_pages.size(), size_t(20));

    // reallocation must reuse freed pages, not extend the file
    pager->begin_txn();
    std::set<uint32_t> reused;
    for (int i = 0; i < 20; ++i) reused.insert(pager->allocate_page());
    pager->commit_txn();
    CHECK_EQ(reused.size(), size_t(20));
    for (size_t i = 0; i < 20; ++i) CHECK(reused.count(ids[i]));
    CHECK_EQ(pager->page_count(), high_water);
    CHECK_EQ(freelist::collect(*pager).size(), size_t(0));
    CHECK_NO_ERROR(freelist::validate(*pager));
}

TEST(pager_reserved_pages_cannot_be_freed) {
    auto pager = Pager::open_memory();
    CHECK_DB_ERROR(pager->free_page(0), int(Err::Pager), "reserved");
    CHECK_DB_ERROR(pager->free_page(1), int(Err::Pager), "reserved");
}

TEST(pager_file_header_validation) {
    // defaults + custom page size
    std::string path = test::temp_db_path();
    std::string path2 = test::temp_db_path();
    {
        auto pager = Pager::open_file(path);
        CHECK_EQ(pager->page_size(), uint32_t(4096));
        auto pager2 = Pager::open_file(path2, 1024);
        CHECK_EQ(pager2->page_size(), uint32_t(1024));
    }
    // bad magic
    {
        std::string path3 = test::temp_db_path();
        {
            auto pager = Pager::open_file(path3);
        }
        FILE* f = fopen(path3.c_str(), "r+b");
        fputc('X', f);
        fclose(f);
        CHECK_DB_ERROR(Pager::open_file(path3), int(Err::Storage), "bad magic");
        std::remove(path3.c_str());
    }
    // truncated file (header claims more pages than exist)
    {
        std::string path4 = test::temp_db_path();
        {
            auto pager = Pager::open_file(path4);
            pager->begin_txn();
            for (int i = 0; i < 10; ++i) pager->allocate_page();
            pager->commit_txn();
            CHECK_EQ(pager->page_count(), uint32_t(12));
        }
        CHECK_EQ(::truncate(path4.c_str(), 5 * 4096), 0);
        CHECK_DB_ERROR(Pager::open_file(path4), int(Err::Storage),
                       "does not match its header");
        std::remove(path4.c_str());
    }
    // header checksum corruption
    {
        std::string path5 = test::temp_db_path();
        {
            auto pager = Pager::open_file(path5);
        }
        FILE* f = fopen(path5.c_str(), "r+b");
        fseek(f, 24, SEEK_SET);   // change counter: inside the checksummed region
        fputc(0xEE, f);
        fclose(f);
        CHECK_DB_ERROR(Pager::open_file(path5), int(Err::Storage),
                       "checksum mismatch");
        std::remove(path5.c_str());
    }
    // impossible page size requests
    CHECK_DB_ERROR(Pager::open_memory(3000), int(Err::Pager), "power of two");
    CHECK_DB_ERROR(Pager::open_memory(256), int(Err::Pager), "power of two");
    std::remove(path.c_str());
    std::remove(path2.c_str());
}

TEST(pager_transaction_state_machine) {
    auto pager = Pager::open_memory();
    CHECK(!pager->in_txn());
    pager->begin_txn();
    CHECK(pager->in_txn());
    CHECK_DB_ERROR(pager->begin_txn(), int(Err::Transaction), "within a transaction");
    pager->commit_txn();
    CHECK(!pager->in_txn());
    CHECK_DB_ERROR(pager->commit_txn(), int(Err::Transaction), "no transaction");
    CHECK_DB_ERROR(pager->rollback_txn(), int(Err::Transaction), "no transaction");
    CHECK_DB_ERROR(pager->begin_stmt(), int(Err::Transaction),
                   "outside a transaction");
}

TEST(pager_writes_outside_transaction_rejected) {
    auto pager = Pager::open_memory();
    // writing outside a transaction puts a dirty page in the cache; a
    // transaction can never open on top of that state (it could not be
    // rolled back), so begin_txn fails loudly instead of guessing
    pager->write_page(1, std::vector<uint8_t>(4096, 1));
    CHECK(pager->is_dirty(1));
    CHECK_DB_ERROR(pager->begin_txn(), int(Err::Pager),
                   "dirty pages outside a transaction");

    // a read-only transaction is a no-op commit
    auto pager2 = Pager::open_memory();
    pager2->begin_txn();
    (void)pager2->get_page(0);
    pager2->commit_txn();
    CHECK(!pager2->in_txn());
}

TEST(pager_rollback_restores_pages_and_truncates) {
    std::string path = test::temp_db_path();
    {
        auto pager = Pager::open_file(path);
        pager->begin_txn();
        uint32_t a = pager->allocate_page();
        std::vector<uint8_t> p(4096, 7);
        pager->write_page(a, p);
        pager->commit_txn();
        CHECK_EQ(pager->page_count(), uint32_t(3));
    }
    {
        auto pager = Pager::open_file(path);
        pager->begin_txn();
        // modify the existing page
        std::vector<uint8_t> p(4096, 9);
        pager->write_page(2, p);
        // extend the file
        for (int i = 0; i < 4; ++i) pager->allocate_page();
        CHECK_EQ(pager->page_count(), uint32_t(7));
        pager->rollback_txn();
        CHECK_EQ(pager->page_count(), uint32_t(3));
        auto back = pager->get_page(2);
        CHECK_EQ(back[100], uint8_t(7));   // original content restored
        CHECK(!journal::exists(path));     // rollback removes any journal
    }
    // the file on disk is also restored
    {
        auto pager = Pager::open_file(path);
        CHECK_EQ(pager->page_count(), uint32_t(3));
        auto back = pager->get_page(2);
        CHECK_EQ(back[100], uint8_t(7));
    }
    std::remove(path.c_str());
}

TEST(pager_statement_savepoints) {
    auto pager = Pager::open_memory();
    pager->begin_txn();
    uint32_t a = pager->allocate_page();
    std::vector<uint8_t> p1(4096, 1);
    pager->write_page(a, p1);

    // statement 1: further modify, then roll back to the savepoint
    pager->begin_stmt();
    std::vector<uint8_t> p2(4096, 2);
    pager->write_page(a, p2);
    uint32_t b = pager->allocate_page();
    CHECK_EQ(pager->page_count(), uint32_t(4));
    pager->rollback_stmt();
    auto back = pager->get_page(a);
    CHECK_EQ(back[0], uint8_t(1));   // statement 1's content survived
    CHECK_EQ(pager->page_count(), uint32_t(3));   // page b never happened
    CHECK(!pager->page_exists(b));

    // statement 2 succeeds; commit keeps it
    pager->begin_stmt();
    std::vector<uint8_t> p3(4096, 3);
    pager->write_page(a, p3);
    pager->end_stmt();
    pager->commit_txn();
    CHECK_EQ(pager->get_page(a)[0], uint8_t(3));
    CHECK_EQ(pager->page_count(), uint32_t(3));
}

TEST(pager_commit_pure_extension) {
    // A transaction that only appends new pages (never touching an
    // existing one) must still persist the extension.
    auto pager = Pager::open_memory();
    pager->begin_txn();
    uint32_t a = pager->allocate_page();
    std::vector<uint8_t> p(4096, 5);
    pager->write_page(a, p);
    pager->commit_txn();
    CHECK_EQ(pager->page_count(), uint32_t(3));
    CHECK_EQ(pager->get_page(a)[4095], uint8_t(5));
    CHECK(!pager->is_dirty(a));
}
