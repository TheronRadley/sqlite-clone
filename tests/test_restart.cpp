// test_restart.cpp — everything a committed session created must survive
// closing and reopening the database file.
#include "test_util.hpp"

#include <cstdio>

#include "database.hpp"
#include "journal.hpp"
#include "pager.hpp"

using namespace sc;

namespace {

std::string count(Database& db, const std::string& t) {
    return db.execute("SELECT COUNT(*) FROM " + t)[0].rows[0][0].display();
}

} // namespace

TEST(restart_schema_and_rows_persist) {
    std::string path = test::temp_db_path();
    {
        auto db = Database::open(path);
        (void)db->execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, "
                          "age INTEGER)");
        (void)db->execute("CREATE TABLE logs (msg TEXT)");
        (void)db->execute("INSERT INTO users VALUES (1, 'Alice', 30), (2, 'Bob', 25)");
        (void)db->execute("INSERT INTO logs VALUES ('hello')");
    }
    {
        auto db = Database::open(path);
        CHECK_EQ(db->table_names().size(), size_t(2));
        std::string schema = db->schema_sql();
        CHECK(schema.find("CREATE TABLE users") != std::string::npos);
        CHECK(schema.find("CREATE TABLE logs") != std::string::npos);
        auto r = db->execute("SELECT name, age FROM users ORDER BY id");
        CHECK_EQ(r[0].rows.size(), size_t(2));
        CHECK_EQ(r[0].rows[0][0].as_text(), std::string("Alice"));
        CHECK_EQ(r[0].rows[1][0].as_text(), std::string("Bob"));
        CHECK_NO_ERROR(db->validate(true));
    }
    std::remove(path.c_str());
}

TEST(restart_index_persists_and_is_used) {
    std::string path = test::temp_db_path();
    {
        auto db = Database::open(path);
        (void)db->execute("CREATE TABLE t (id INTEGER PRIMARY KEY, n INTEGER)");
        for (int i = 1; i <= 100; ++i)
            (void)db->execute("INSERT INTO t VALUES (" + std::to_string(i) + ", " +
                              std::to_string(i % 10) + ")");
        (void)db->execute("CREATE INDEX t_n ON t(n)");
    }
    {
        auto db = Database::open(path);
        CHECK_EQ(db->index_names().size(), size_t(1));
        CHECK_EQ(db->index_names()[0], std::string("t_n"));
        // the index still answers correctly...
        auto r = db->execute("SELECT id FROM t WHERE n = 3 ORDER BY id");
        CHECK_EQ(r[0].rows.size(), size_t(10));
        CHECK_EQ(r[0].rows[0][0].as_int(), int64_t(3));
        // ...and the planner still picks it
        auto ex = db->execute("EXPLAIN SELECT id FROM t WHERE n = 3");
        CHECK(ex[0].message.find("t_n") != std::string::npos);
        CHECK_NO_ERROR(db->validate(true));   // deep: index vs table agreement
    }
    std::remove(path.c_str());
}

TEST(restart_auto_rowid_continues) {
    std::string path = test::temp_db_path();
    {
        auto db = Database::open(path);
        (void)db->execute("CREATE TABLE t (id INTEGER PRIMARY KEY, s TEXT)");
        (void)db->execute("INSERT INTO t VALUES (5, 'a'), (9, 'b')");
    }
    {
        auto db = Database::open(path);
        (void)db->execute("INSERT INTO t VALUES (NULL, 'c')");   // gets 10
        auto r = db->execute("SELECT id, s FROM t ORDER BY id");
        CHECK_EQ(r[0].rows.size(), size_t(3));
        CHECK_EQ(r[0].rows[2][0].as_int(), int64_t(10));
    }
    {
        auto db = Database::open(path);
        auto r = db->execute("SELECT id FROM t ORDER BY id DESC LIMIT 1");
        CHECK_EQ(r[0].rows[0][0].as_int(), int64_t(10));
    }
    std::remove(path.c_str());
}

TEST(restart_freed_pages_are_reused) {
    std::string path = test::temp_db_path();
    uint32_t pages_before = 0;
    {
        auto db = Database::open(path);
        (void)db->execute("CREATE TABLE t (id INTEGER PRIMARY KEY, s TEXT)");
        for (int i = 1; i <= 400; ++i)
            (void)db->execute("INSERT INTO t VALUES (" + std::to_string(i) +
                              ", 'payload-" + std::to_string(i) + "')");
        (void)db->execute("DELETE FROM t WHERE id > 5");
        CHECK_NO_ERROR(db->validate(true));
        pages_before = db->pager().page_count();
    }
    {
        auto db = Database::open(path);
        CHECK_EQ(db->pager().page_count(), pages_before);
        // re-growing the table must draw from the freelist, not extend the file
        for (int i = 1; i <= 200; ++i)
            (void)db->execute("INSERT INTO t VALUES (" + std::to_string(500 + i) +
                              ", 'again-" + std::to_string(i) + "')");
        CHECK(db->pager().page_count() <= pages_before);
        CHECK_NO_ERROR(db->validate(true));
        CHECK_EQ(count(*db, "t"), std::string("205"));
    }
    std::remove(path.c_str());
}

TEST(restart_rolled_back_txn_leaves_no_trace) {
    std::string path = test::temp_db_path();
    {
        auto db = Database::open(path);
        (void)db->execute("CREATE TABLE t (a INTEGER)");
        (void)db->execute("INSERT INTO t VALUES (1)");
        (void)db->execute("BEGIN");
        (void)db->execute("INSERT INTO t VALUES (2), (3), (4)");
        (void)db->execute("ROLLBACK");
    }
    CHECK(!journal::exists(path));
    {
        auto db = Database::open(path);
        CHECK_EQ(count(*db, "t"), std::string("1"));
        CHECK_NO_ERROR(db->validate(true));
    }
    std::remove(path.c_str());
}

TEST(restart_many_sessions_accumulate) {
    std::string path = test::temp_db_path();
    for (int session = 0; session < 8; ++session) {
        auto db = Database::open(path);
        if (session == 0)
            (void)db->execute("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)");
        for (int i = 0; i < 25; ++i)
            (void)db->execute("INSERT INTO t VALUES (" +
                              std::to_string(session * 25 + i + 1) + ", " +
                              std::to_string(session) + ")");
    }
    {
        auto db = Database::open(path);
        CHECK_EQ(count(*db, "t"), std::string("200"));
        auto r = db->execute("SELECT COUNT(*) FROM t WHERE v = 3");
        CHECK_EQ(r[0].rows[0][0].as_int(), int64_t(25));
        CHECK_NO_ERROR(db->validate(true));
    }
    std::remove(path.c_str());
}
