// test_transactions.cpp — transaction semantics, statement-level
// rollback, crash simulation, and rollback-journal recovery.
#include "test_util.hpp"

#include <cstdio>

#include "database.hpp"
#include "journal.hpp"
#include "pager.hpp"

using namespace sc;

namespace {

void run(Database& db, const std::string& sql) { (void)db.execute(sql); }

std::string count_of(Database& db, const std::string& table) {
    return db.execute("SELECT COUNT(*) FROM " + table)[0].rows[0][0].display();
}

} // namespace

TEST(txn_autocommit_persists_per_statement) {
    std::string path = test::temp_db_path();
    {
        auto db = Database::open(path);
        run(*db, "CREATE TABLE t (a INTEGER)");
        run(*db, "INSERT INTO t VALUES (1)");
        run(*db, "INSERT INTO t VALUES (2)");
        CHECK_EQ(count_of(*db, "t"), std::string("2"));
        CHECK(!db->in_txn());   // autocommit: nothing left open
    }
    {
        auto db = Database::open(path);
        CHECK_EQ(count_of(*db, "t"), std::string("2"));
    }
    std::remove(path.c_str());
}

TEST(txn_commit_and_rollback) {
    std::string path = test::temp_db_path();
    {
        auto db = Database::open(path);
        run(*db, "CREATE TABLE t (a INTEGER)");
        run(*db, "INSERT INTO t VALUES (1)");
        run(*db, "BEGIN");
        CHECK(db->in_txn());
        run(*db, "INSERT INTO t VALUES (2)");
        run(*db, "INSERT INTO t VALUES (3)");
        CHECK_EQ(count_of(*db, "t"), std::string("3"));   // visible inside
        run(*db, "COMMIT");
        CHECK(!db->in_txn());
        run(*db, "BEGIN");
        run(*db, "INSERT INTO t VALUES (4)");
        run(*db, "ROLLBACK");
        CHECK_EQ(count_of(*db, "t"), std::string("3"));
    }
    {
        auto db = Database::open(path);
        CHECK_EQ(count_of(*db, "t"), std::string("3"));   // COMMIT kept, ROLLBACK gone
    }
    std::remove(path.c_str());
}

TEST(txn_protocol_errors) {
    auto db = Database::open_memory();
    run(*db, "CREATE TABLE t (a INTEGER)");
    CHECK_DB_ERROR(run(*db, "COMMIT"), int(Err::Transaction), "no transaction");
    CHECK_DB_ERROR(run(*db, "ROLLBACK"), int(Err::Transaction), "no transaction");
    run(*db, "BEGIN");
    CHECK_DB_ERROR(run(*db, "BEGIN"), int(Err::Transaction),
                   "within a transaction");
    run(*db, "ROLLBACK");
    CHECK(!db->in_txn());
}

TEST(txn_statement_rollback_inside_transaction) {
    std::string path = test::temp_db_path();
    {
        auto db = Database::open(path);
        run(*db, "CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER)");
        run(*db, "BEGIN");
        run(*db, "INSERT INTO t VALUES (1, 10)");
        // a failing statement inside the transaction rolls back only itself
        CHECK_DB_ERROR(run(*db, "INSERT INTO t VALUES (1, 99)"), int(Err::Constraint),
                       "already contains value 1");
        CHECK(db->in_txn());   // the transaction survives
        run(*db, "INSERT INTO t VALUES (2, 20)");
        // a failed statement that modified pages first (duplicate insert
        // after an index update) must leave no partial state
        CHECK_DB_ERROR(run(*db, "INSERT INTO t VALUES (3, 30), (3, 33)"),
                       int(Err::Constraint), "already contains value 3");
        CHECK_EQ(count_of(*db, "t"), std::string("2"));
        run(*db, "COMMIT");
        CHECK_EQ(count_of(*db, "t"), std::string("2"));
    }
    {
        auto db = Database::open(path);
        CHECK_EQ(count_of(*db, "t"), std::string("2"));
        CHECK_NO_ERROR(db->validate(true));
    }
    std::remove(path.c_str());
}

TEST(txn_crash_mid_transaction_leaves_file_untouched) {
    std::string path = test::temp_db_path();
    {
        auto db = Database::open(path);
        run(*db, "CREATE TABLE t (a INTEGER)");
        run(*db, "INSERT INTO t VALUES (1)");
    }
    uint64_t size_before = 0;
    {
        std::FILE* f = fopen(path.c_str(), "rb");
        fseek(f, 0, SEEK_END);
        size_before = uint64_t(ftell(f));
        fclose(f);
    }
    {
        // open, start a transaction, insert many rows, then simply
        // destroy the database: uncommitted work must vanish (the file
        // can only ever contain committed page images)
        auto db = Database::open(path);
        run(*db, "BEGIN");
        for (int i = 0; i < 200; ++i)
            run(*db, "INSERT INTO t VALUES (" + std::to_string(i) + ")");
        CHECK_EQ(count_of(*db, "t"), std::string("201"));
    }
    {
        std::FILE* f = fopen(path.c_str(), "rb");
        fseek(f, 0, SEEK_END);
        CHECK_EQ(uint64_t(ftell(f)), size_before);   // byte-identical file
        fclose(f);
        CHECK(!journal::exists(path));
        auto db = Database::open(path);
        CHECK_EQ(count_of(*db, "t"), std::string("1"));
        CHECK_NO_ERROR(db->validate(true));
    }
    std::remove(path.c_str());
}

TEST(txn_journal_recovers_interrupted_commit) {
    std::string path = test::temp_db_path();
    {
        auto db = Database::open(path);
        run(*db, "CREATE TABLE t (a INTEGER)");
        run(*db, "INSERT INTO t VALUES (1), (2), (3)");
    }
    std::vector<uint8_t> page0_before, page2_before;
    uint32_t count_before = 0;
    {
        // simulate a crash *between* the journal write and the journal
        // deletion of a commit: reproduce the commit steps by hand on a
        // raw pager, flush, then abandon without removing the journal
        auto pager = Pager::open_file(path);
        count_before = pager->page_count();
        page0_before = pager->store_read(0);
        page2_before = pager->store_read(2);

        pager->begin_txn();
        // "modify" the table's root page and extend the file, as a real
        // INSERT would
        std::vector<uint8_t> mod = page2_before;
        for (auto& b : mod) b ^= 0xff;
        pager->write_page(2, mod);
        pager->allocate_page();
        // refresh the header like commit_txn does
        auto fresh = hdr::make_image(pager->page_size(), pager->page_count(), 0, 0,
                                     99);
        pager->write_page(0, fresh);

        JournalData jd;
        jd.page_size = pager->page_size();
        jd.original_page_count = count_before;
        jd.pages[0] = page0_before;
        jd.pages[2] = page2_before;
        journal::write(path, jd);
        pager->flush_all();   // dirty (uncommitted) pages reach the file
        // no commit, no journal removal, pager destroyed: crash
    }
    CHECK(journal::exists(path));
    {
        // opening again must run recovery: original images restored, the
        // file truncated back to its committed size
        auto db = Database::open(path);
        CHECK_EQ(count_of(*db, "t"), std::string("3"));
        CHECK_EQ(db->pager().page_count(), count_before);
        auto raw = Pager::open_file(path);   // const facade: read via a raw pager
        CHECK(raw->store_read(2) == page2_before);
        CHECK_NO_ERROR(db->validate(true));
        CHECK(!journal::exists(path));   // recovery removed the journal
    }
    std::remove(path.c_str());
}

TEST(txn_corrupt_journal_is_a_hard_error) {
    std::string path = test::temp_db_path();
    {
        auto db = Database::open(path);
        run(*db, "CREATE TABLE t (a INTEGER)");
        run(*db, "INSERT INTO t VALUES (1)");
    }
    std::string jpath = journal::journal_path_for(path);
    {
        // stage a valid journal describing an interrupted commit
        auto pager = Pager::open_file(path);
        auto p0 = pager->store_read(0);
        auto p2 = pager->store_read(2);
        std::vector<uint8_t> mod = p2;
        for (auto& b : mod) b ^= 0xff;
        pager->begin_txn();
        pager->write_page(2, mod);
        JournalData jd;
        jd.page_size = pager->page_size();
        jd.original_page_count = pager->page_count();
        jd.pages[0] = p0;
        jd.pages[2] = p2;
        journal::write(path, jd);
        pager->flush_all();
    }
    CHECK(journal::exists(path));
    // corrupt a byte inside the record area (beyond the 32-byte header)
    {
        std::FILE* f = fopen(jpath.c_str(), "r+b");
        fseek(f, 40, SEEK_SET);
        int c = fgetc(f);
        fseek(f, 40, SEEK_SET);
        fputc(c ^ 0x55, f);
        fclose(f);
    }
    // a corrupt journal is never guessed about: opening fails loudly
    CHECK_DB_ERROR(Database::open(path), int(Err::Storage), "journal");
    std::remove(jpath.c_str());
    std::remove(path.c_str());
}

TEST(txn_journal_read_and_validate_shape) {
    std::string path = test::temp_db_path();
    {
        auto db = Database::open(path);
        run(*db, "CREATE TABLE t (a INTEGER)");
    }
    std::string jpath = journal::journal_path_for(path);
    {
        auto pager = Pager::open_file(path);
        JournalData jd;
        jd.page_size = pager->page_size();
        jd.original_page_count = pager->page_count();
        jd.pages[0] = pager->store_read(0);
        journal::write(path, jd);
    }
    CHECK(journal::exists(path));
    {
        JournalData back = journal::read_and_validate(path);
        CHECK_EQ(back.page_size, uint32_t(4096));
        CHECK_EQ(back.original_page_count, uint32_t(3));   // header, catalog, table root
        CHECK_EQ(back.pages.size(), size_t(1));
    }
    // truncated journal header
    {
        std::FILE* f = fopen(jpath.c_str(), "r+b");
        fseek(f, 0, SEEK_SET);
        fputc('X', f);
        fclose(f);
        CHECK_DB_ERROR(journal::read_and_validate(path), int(Err::Storage),
                       "journal");
    }
    std::remove(jpath.c_str());
    std::remove(path.c_str());
}
