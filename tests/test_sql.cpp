// test_sql.cpp — end-to-end SQL tests through the Database facade
// (parser -> planner -> executor -> btree -> pager), on in-memory
// databases. Persistence is covered by test_restart.cpp.
#include "test_util.hpp"

#include "database.hpp"
#include "pager.hpp"

using namespace sc;

namespace {

// execute and require exactly one result
ExecResult one(Database& db, const std::string& sql) {
    auto rs = db.execute(sql);
    if (rs.size() != 1)
        throw ::test::TestFailure(sc::str("expected 1 statement result, got ",
                                          rs.size(), " for: ", sql));
    return rs[0];
}

// render a SELECT result as "v|v|v" rows joined by newlines
std::string rows(Database& db, const std::string& sql) {
    ExecResult r = one(db, sql);
    if (!r.is_rows)
        throw ::test::TestFailure(sc::str("expected rows from: ", sql));
    std::string out;
    for (const auto& row : r.rows) {
        for (size_t i = 0; i < row.size(); ++i) {
            if (i) out += "|";
            out += row[i].display();
        }
        out += "\n";
    }
    return out;
}

void check_rows(Database& db, const std::string& sql, const std::string& want) {
    std::string got = rows(db, sql);
    if (got != want)
        throw ::test::TestFailure(sc::str(__FILE__, ":\"", sql, "\" produced:\n", got,
                                          "\nexpected:\n", want));
}

} // namespace

TEST(sql_create_insert_select) {
    auto db = Database::open_memory();
    (void)one(*db, "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)");
    (void)one(*db, "INSERT INTO users VALUES (1, 'Alice', 30), (2, 'Bob', 25)");
    check_rows(*db, "SELECT * FROM users",
               "1|Alice|30\n"
               "2|Bob|25\n");
    check_rows(*db, "SELECT name, age FROM users WHERE id = 2", "Bob|25\n");
    CHECK_EQ(db->table_names().size(), size_t(1));
    CHECK_EQ(db->table_names()[0], std::string("users"));
}

TEST(sql_expressions_in_projection) {
    auto db = Database::open_memory();
    (void)one(*db, "CREATE TABLE t (a INTEGER)");
    (void)one(*db, "INSERT INTO t VALUES (7)");
    check_rows(*db, "SELECT a, a + 1, a * 2 - 3, -a, a % 4, a / 2 FROM t",
               "7|8|11|-7|3|3\n");
    // from-less SELECT evaluates pure expressions
    check_rows(*db, "SELECT 1 + 2, 'hi', NULL", "3|hi|\n");
}

TEST(sql_where_operators) {
    auto db = Database::open_memory();
    (void)one(*db, "CREATE TABLE t (a INTEGER, b TEXT)");
    (void)one(*db, "INSERT INTO t VALUES (1, 'x'), (2, 'y'), (3, 'x'), (10, 'z')");
    check_rows(*db, "SELECT a FROM t WHERE a = 2", "2\n");
    check_rows(*db, "SELECT a FROM t WHERE a != 2 ORDER BY a", "1\n3\n10\n");
    check_rows(*db, "SELECT a FROM t WHERE a <> 2 ORDER BY a", "1\n3\n10\n");
    check_rows(*db, "SELECT a FROM t WHERE a < 3 ORDER BY a", "1\n2\n");
    check_rows(*db, "SELECT a FROM t WHERE a <= 3 ORDER BY a", "1\n2\n3\n");
    check_rows(*db, "SELECT a FROM t WHERE a > 2 ORDER BY a", "3\n10\n");
    check_rows(*db, "SELECT a FROM t WHERE a >= 3 ORDER BY a", "3\n10\n");
    check_rows(*db, "SELECT a FROM t WHERE b = 'x' AND a > 1", "3\n");
    check_rows(*db, "SELECT a FROM t WHERE b = 'z' OR a = 1 ORDER BY a", "1\n10\n");
    check_rows(*db, "SELECT a FROM t WHERE NOT b = 'x' ORDER BY a", "2\n10\n");
    // AND binds tighter than OR
    check_rows(*db, "SELECT a FROM t WHERE b = 'x' AND a = 1 OR a = 10", "1\n10\n");
    // text comparison is bytewise
    check_rows(*db, "SELECT a FROM t WHERE b < 'y' ORDER BY a", "1\n3\n");
    check_rows(*db, "SELECT a FROM t WHERE b >= 'y'", "2\n10\n");
}

TEST(sql_null_semantics) {
    auto db = Database::open_memory();
    (void)one(*db, "CREATE TABLE t (a INTEGER, b TEXT)");
    (void)one(*db, "INSERT INTO t VALUES (1, 'x'), (NULL, 'y'), (3, NULL)");
    // SQL equality with NULL never matches
    check_rows(*db, "SELECT a FROM t WHERE a = NULL", "");
    check_rows(*db, "SELECT a FROM t WHERE NULL = NULL", "");
    check_rows(*db, "SELECT a FROM t WHERE a IS NULL", "\n");
    check_rows(*db, "SELECT a, b FROM t WHERE b IS NOT NULL ORDER BY a",
               "|y\n"
               "1|x\n");
    // division by zero yields NULL (row filtered out)
    check_rows(*db, "SELECT a FROM t WHERE 1 / 0 IS NULL ORDER BY a", "\n1\n3\n");
    // aggregates skip NULLs; COUNT(*) does not
    check_rows(*db, "SELECT COUNT(*), COUNT(a), COUNT(b) FROM t", "3|2|2\n");
    check_rows(*db, "SELECT MIN(a), MAX(a), SUM(a) FROM t", "1|3|4\n");
    // NULLs sort first (structural order)
    check_rows(*db, "SELECT a FROM t ORDER BY a", "\n1\n3\n");
    // unlisted columns default to NULL
    (void)one(*db, "INSERT INTO t (a) VALUES (9)");
    check_rows(*db, "SELECT b FROM t WHERE a = 9", "\n");
}

TEST(sql_order_limit_offset) {
    auto db = Database::open_memory();
    (void)one(*db, "CREATE TABLE t (a INTEGER, b TEXT)");
    for (int i = 1; i <= 10; ++i)
        (void)one(*db, "INSERT INTO t VALUES (" + std::to_string(i) +
                           ", 'v" + std::to_string(i % 3) + "')");
    check_rows(*db, "SELECT a FROM t ORDER BY a DESC LIMIT 3", "10\n9\n8\n");
    check_rows(*db, "SELECT a FROM t ORDER BY a LIMIT 2 OFFSET 3", "4\n5\n");
    check_rows(*db, "SELECT a FROM t ORDER BY a LIMIT 100",  // LIMIT beyond the end
               "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n");
    check_rows(*db, "SELECT a FROM t ORDER BY a LIMIT 3 OFFSET 100", "");
    check_rows(*db, "SELECT a FROM t ORDER BY a LIMIT 0", "");
    // multi-key ordering with mixed directions
    check_rows(*db, "SELECT a, b FROM t ORDER BY b DESC, a ASC LIMIT 4",
               "2|v2\n5|v2\n8|v2\n1|v1\n");
    // ORDER BY a column that is not projected
    check_rows(*db, "SELECT b FROM t ORDER BY a DESC LIMIT 2", "v1\nv0\n");
}

TEST(sql_aggregates) {
    auto db = Database::open_memory();
    (void)one(*db, "CREATE TABLE t (a INTEGER, s TEXT)");
    (void)one(*db, "INSERT INTO t VALUES (5, 'e'), (2, 'b'), (9, 'i'), (NULL, 'n')");
    check_rows(*db, "SELECT COUNT(*) FROM t", "4\n");
    check_rows(*db, "SELECT COUNT(a) FROM t", "3\n");
    check_rows(*db, "SELECT MIN(a), MAX(a), SUM(a) FROM t", "2|9|16\n");
    check_rows(*db, "SELECT MIN(s), MAX(s) FROM t", "b|n\n");
    // empty table
    (void)one(*db, "CREATE TABLE e (a INTEGER)");
    check_rows(*db, "SELECT COUNT(*), COUNT(a), MIN(a), MAX(a), SUM(a) FROM e",
               "0|0|||\n");
    // aggregates are not supported without FROM
    CHECK_DB_ERROR(one(*db, "SELECT COUNT(*)"), int(Err::Execution), "without FROM");
}

TEST(sql_update) {
    auto db = Database::open_memory();
    (void)one(*db, "CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b TEXT)");
    (void)one(*db, "INSERT INTO t VALUES (1, 10, 'x'), (2, 20, 'y'), (3, 30, 'z')");
    ExecResult r = one(*db, "UPDATE t SET a = a + 1 WHERE a >= 20");
    CHECK_EQ(r.message, std::string("2 row(s) updated in 't'"));
    check_rows(*db, "SELECT a FROM t ORDER BY id", "10\n21\n31\n");
    // multiple assignments
    (void)one(*db, "UPDATE t SET a = 99, b = 'w' WHERE id = 1");
    check_rows(*db, "SELECT a, b FROM t WHERE id = 1", "99|w\n");
    // no match is not an error
    r = one(*db, "UPDATE t SET a = 0 WHERE id = 777");
    CHECK_EQ(r.message, std::string("0 row(s) updated in 't'"));
    // primary key can move
    (void)one(*db, "UPDATE t SET id = 50 WHERE id = 3");
    check_rows(*db, "SELECT id FROM t ORDER BY id", "1\n2\n50\n");
    // ... but not onto an existing value
    CHECK_DB_ERROR(one(*db, "UPDATE t SET id = 1 WHERE id = 2"), int(Err::Constraint),
                   "already contains value 1");
    // the failed update changed nothing
    check_rows(*db, "SELECT id FROM t ORDER BY id", "1\n2\n50\n");
}

TEST(sql_delete) {
    auto db = Database::open_memory();
    (void)one(*db, "CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER)");
    (void)one(*db, "INSERT INTO t VALUES (1, 10), (2, 20), (3, 30), (4, 40)");
    ExecResult r = one(*db, "DELETE FROM t WHERE a > 15");
    CHECK_EQ(r.message, std::string("3 row(s) deleted from 't'"));
    check_rows(*db, "SELECT * FROM t", "1|10\n");
    r = one(*db, "DELETE FROM t WHERE a = 999");
    CHECK_EQ(r.message, std::string("0 row(s) deleted from 't'"));
    (void)one(*db, "DELETE FROM t");
    check_rows(*db, "SELECT COUNT(*) FROM t", "0\n");
    // auto-assigned rowids restart from max+1 of what remains (nothing)
    (void)one(*db, "INSERT INTO t (a) VALUES (7)");
    check_rows(*db, "SELECT * FROM t", "1|7\n");
}

TEST(sql_auto_rowid_and_explicit) {
    auto db = Database::open_memory();
    (void)one(*db, "CREATE TABLE t (id INTEGER PRIMARY KEY, s TEXT)");
    (void)one(*db, "INSERT INTO t VALUES (NULL, 'a'), (NULL, 'b')");
    check_rows(*db, "SELECT id, s FROM t", "1|a\n2|b\n");
    // explicit ids interleave with auto-assignment
    (void)one(*db, "INSERT INTO t VALUES (10, 'c')");
    (void)one(*db, "INSERT INTO t VALUES (NULL, 'd')");
    check_rows(*db, "SELECT id, s FROM t ORDER BY id", "1|a\n2|b\n10|c\n11|d\n");
    // tables without a declared PRIMARY KEY still have rowids
    (void)one(*db, "CREATE TABLE plain (x INTEGER)");
    (void)one(*db, "INSERT INTO plain VALUES (1), (2)");
    check_rows(*db, "SELECT x FROM plain", "1\n2\n");
}

TEST(sql_type_affinity) {
    auto db = Database::open_memory();
    (void)one(*db, "CREATE TABLE t (a INTEGER, b TEXT)");
    // well-formed integer text is accepted into INTEGER columns
    (void)one(*db, "INSERT INTO t (a) VALUES ('42')");
    check_rows(*db, "SELECT a FROM t WHERE a = 42", "42\n");
    // integers are rendered into TEXT columns
    (void)one(*db, "INSERT INTO t (b) VALUES (99)");
    check_rows(*db, "SELECT b FROM t WHERE b = '99'", "99\n");
    // non-numeric text is rejected by INTEGER columns
    CHECK_DB_ERROR(one(*db, "INSERT INTO t (a) VALUES ('not a number')"),
                   int(Err::Type), "TEXT");
    // NULL passes through
    (void)one(*db, "INSERT INTO t VALUES (NULL, NULL)");
    check_rows(*db, "SELECT COUNT(*) FROM t WHERE a IS NULL AND b IS NULL", "1\n");
}

TEST(sql_primary_key_constraints) {
    auto db = Database::open_memory();
    (void)one(*db, "CREATE TABLE t (id INTEGER PRIMARY KEY, s TEXT)");
    (void)one(*db, "INSERT INTO t VALUES (1, 'a')");
    CHECK_DB_ERROR(one(*db, "INSERT INTO t VALUES (1, 'b')"), int(Err::Constraint),
                   "already contains value 1");
    CHECK_DB_ERROR(one(*db, "INSERT INTO t VALUES ('x', 'c')"), int(Err::Type),
                   "cannot store TEXT");
    // storage-layer uniqueness: one row per key even after updates
    (void)one(*db, "INSERT INTO t VALUES (2, 'b')");
    (void)one(*db, "UPDATE t SET id = 20 WHERE id = 2");
    (void)one(*db, "INSERT INTO t VALUES (2, 'c')");   // 2 is free again now
    check_rows(*db, "SELECT id, s FROM t ORDER BY id", "1|a\n2|c\n20|b\n");
}

TEST(sql_statement_errors) {
    auto db = Database::open_memory();
    (void)one(*db, "CREATE TABLE t (a INTEGER)");
    CHECK_DB_ERROR(one(*db, "SELECT * FROM missing"), int(Err::Execution),
                   "no such table: missing");
    CHECK_DB_ERROR(one(*db, "SELECT nosuch FROM t"), int(Err::Execution),
                   "no such column: nosuch");
    CHECK_DB_ERROR(one(*db, "INSERT INTO t VALUES (1, 2)"), int(Err::Execution),
                   "expects 1 values per row, got 2");
    CHECK_DB_ERROR(one(*db, "CREATE TABLE t (a INTEGER)"), int(Err::Constraint),
                   "already exists");
    CHECK_DB_ERROR(one(*db, "UPDATE missing SET a = 1"), int(Err::Execution),
                   "no such table: missing");
    CHECK_DB_ERROR(one(*db, "DELETE FROM missing"), int(Err::Execution),
                   "no such table: missing");
    CHECK_DB_ERROR(one(*db, "SELECT * FORM t"), int(Err::Parse), "parse error");
    CHECK_DB_ERROR(one(*db, "SELECT * FROM t WHERE"), int(Err::Parse), "parse error");
    // a failed statement leaves the database usable and unchanged
    check_rows(*db, "SELECT COUNT(*) FROM t", "0\n");
    (void)one(*db, "INSERT INTO t VALUES (5)");
    check_rows(*db, "SELECT a FROM t", "5\n");
}

TEST(sql_index_create_and_use) {
    auto db = Database::open_memory();
    (void)one(*db, "CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER)");
    for (int i = 1; i <= 40; ++i)
        (void)one(*db, "INSERT INTO t VALUES (" + std::to_string(i) + ", " +
                           std::to_string(i % 7) + ")");
    ExecResult r = one(*db, "CREATE INDEX t_a ON t(a)");
    CHECK_EQ(r.message, std::string("index 't_a' created over 40 row(s)"));
    CHECK_EQ(db->index_names().size(), size_t(1));
    CHECK_EQ(db->index_names()[0], std::string("t_a"));

    // indexed queries agree with full scans
    check_rows(*db, "SELECT id FROM t WHERE a = 3 ORDER BY id",
               "3\n10\n17\n24\n31\n38\n");
    // ranges
    check_rows(*db, "SELECT COUNT(*) FROM t WHERE a >= 5", "11\n");   // a = i%7: only 5 and 6 qualify
    // EXPLAIN shows the index being used
    ExecResult ex = one(*db, "EXPLAIN SELECT id FROM t WHERE a = 3");
    CHECK(!ex.is_rows);
    CHECK(ex.message.find("Index Lookup") != std::string::npos);
    CHECK(ex.message.find("t_a") != std::string::npos);
    ex = one(*db, "EXPLAIN SELECT id FROM t WHERE a > 3");
    CHECK(ex.message.find("Index Scan") != std::string::npos);
    CHECK(ex.message.find("Range: a > 3") != std::string::npos);
    // the schema round-trips
    std::string schema = db->schema_sql();
    CHECK(schema.find("CREATE INDEX t_a ON t(a)") != std::string::npos);
    // deep validation checks index/table consistency
    CHECK_NO_ERROR(db->validate(true));
}

TEST(sql_index_maintenance_on_update_delete) {
    auto db = Database::open_memory();
    (void)one(*db, "CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER)");
    (void)one(*db, "INSERT INTO t VALUES (1, 10), (2, 20), (3, 30)");
    (void)one(*db, "CREATE INDEX t_a ON t(a)");
    // UPDATE moves the index entry
    (void)one(*db, "UPDATE t SET a = 21 WHERE id = 2");
    check_rows(*db, "SELECT id FROM t WHERE a = 20", "");
    check_rows(*db, "SELECT id FROM t WHERE a = 21", "2\n");
    // DELETE removes it
    (void)one(*db, "DELETE FROM t WHERE a = 30");
    check_rows(*db, "SELECT id FROM t WHERE a = 30", "");
    CHECK_NO_ERROR(db->validate(true));
    check_rows(*db, "SELECT id FROM t ORDER BY a", "1\n2\n");
}

TEST(sql_dump_roundtrip) {
    auto db = Database::open_memory();
    (void)one(*db, "CREATE TABLE t (id INTEGER PRIMARY KEY, s TEXT, n INTEGER)");
    (void)one(*db, "INSERT INTO t VALUES (1, 'has ''quotes''', 10), (2, NULL, NULL)");
    (void)one(*db, "CREATE INDEX t_n ON t(n)");
    std::string dump = db->dump_sql();
    CHECK(dump.find("CREATE TABLE t") != std::string::npos);
    CHECK(dump.find("CREATE INDEX t_n ON t(n)") != std::string::npos);
    CHECK(dump.find("'has ''quotes'''") != std::string::npos);

    // replaying the dump in a fresh database reproduces the data
    auto db2 = Database::open_memory();
    (void)db2->execute(dump);
    std::string a = rows(*db, "SELECT * FROM t ORDER BY id");
    std::string b = rows(*db2, "SELECT * FROM t ORDER BY id");
    CHECK_EQ(a, b);
    CHECK_NO_ERROR(db2->validate(true));
}

TEST(sql_multi_statement_execute) {
    auto db = Database::open_memory();
    auto rs = db->execute("CREATE TABLE t (a INTEGER); INSERT INTO t VALUES (1), (2); "
                          "SELECT a FROM t ORDER BY a;");
    CHECK_EQ(rs.size(), size_t(3));
    CHECK(!rs[0].is_rows);
    CHECK(!rs[1].is_rows);
    CHECK(rs[2].is_rows);
    CHECK_EQ(rs[2].rows.size(), size_t(2));
    CHECK_EQ(rs[2].rows[1][0].as_int(), int64_t(2));
}

TEST(sql_large_table_end_to_end) {
    // enough rows to force splits, merges, and multi-level trees
    auto db = Database::open_memory(1024);
    (void)one(*db, "CREATE TABLE t (id INTEGER PRIMARY KEY, s TEXT)");
    for (int i = 1; i <= 400; ++i)
        (void)one(*db, "INSERT INTO t VALUES (" + std::to_string(i) + ", 'row" +
                           std::to_string(i) + "')");
    CHECK_NO_ERROR(db->validate(true));
    check_rows(*db, "SELECT COUNT(*) FROM t", "400\n");
    check_rows(*db, "SELECT s FROM t WHERE id = 257", "row257\n");
    // delete a swath, then verify the survivors
    (void)one(*db, "DELETE FROM t WHERE id <= 300");
    CHECK_NO_ERROR(db->validate(true));
    check_rows(*db, "SELECT COUNT(*), MIN(id), MAX(id) FROM t", "100|301|400\n");
    (void)one(*db, "CREATE INDEX t_s ON t(s)");
    check_rows(*db, "SELECT id FROM t WHERE s = 'row350'", "350\n");
    CHECK_NO_ERROR(db->validate(true));
}
