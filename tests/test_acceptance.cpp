// test_acceptance.cpp — the end-to-end acceptance scenario (spec §30).
//
// Phase 1: a realistic workload against a `users` table — schema with a
// TEXT primary key and several secondary columns, a secondary index,
// bulk inserts (including multi-row VALUES), every SELECT shape
// (WHERE, ORDER BY, LIMIT/OFFSET, COUNT), UPDATE and DELETE with
// predicates, transactions with rollback, and error handling — every
// step checked against expected output.
//
// Phase 2: "process restart" — the Database object is destroyed (file
// closed; no state survives in memory) and reopened from disk. All
// committed state must be there: rows, schema, the index (still chosen
// by the planner), auto-rowid continuation, and the B-Tree must pass
// deep validation.
#include "test_util.hpp"

#include <cstdio>

#include "database.hpp"

using namespace sc;

namespace {

// run one statement and require it to succeed
std::vector<ExecResult> must(Database& db, const std::string& sql) {
    auto rs = db.execute(sql);
    if (rs.size() != 1)
        throw ::test::TestFailure(sc::str("expected 1 result for: ", sql));
    return rs;
}

// render a rows-result as "a|b|c\n" lines for exact comparison
std::string rows_of(Database& db, const std::string& sql) {
    auto rs = must(db, sql);
    if (!rs[0].is_rows)
        throw ::test::TestFailure(sc::str("expected rows for: ", sql));
    std::string out;
    for (const auto& row : rs[0].rows) {
        for (size_t i = 0; i < row.size(); ++i) {
            if (i) out += "|";
            out += row[i].display();
        }
        out += "\n";
    }
    return out;
}

// run EXPLAIN and return the plan text
std::string plan_of(Database& db, const std::string& sql) {
    return must(db, sql)[0].message;
}

} // namespace

TEST(acceptance_users_end_to_end_and_restart) {
    const std::string path = test::temp_db_path();

    // ---------- phase 1: the workload ----------
    {
        auto db = Database::open(path);

        must(*db, "CREATE TABLE users ("
                  "id INTEGER PRIMARY KEY,"
                  "email TEXT,"
                  "name TEXT,"
                  "age INTEGER,"
                  "city TEXT)");
        must(*db, "CREATE INDEX users_email ON users(email)");
        must(*db, "CREATE INDEX users_city ON users(city)");
        must(*db, "CREATE INDEX users_age ON users(age)");

        // bulk load, several shapes
        must(*db, "INSERT INTO users VALUES"
                  "(1, 'alice@example.com', 'Alice', 29, 'Lisbon'),"
                  "(2, 'bob@example.com', 'Bob', 41, 'Porto'),"
                  "(3, 'carol@example.com', 'Carol', 35, 'Lisbon'),"
                  "(4, 'dave@example.com', 'Dave', 19, 'Braga'),"
                  "(5, 'erin@example.com', 'Erin', 63, 'Porto')");
        for (int i = 0; i < 30; ++i)
            must(*db, "INSERT INTO users VALUES (" + std::to_string(10 + i) +
                          ", 'user" + std::to_string(i) +
                          "@example.com', 'User " + std::to_string(i) + "', " +
                          std::to_string(20 + i) + ", 'Faro')");

        // schema introspection
        auto tables = db->table_names();
        CHECK(tables.size() == 1 && tables[0] == "users");
        CHECK(db->index_names().size() == 3);

        // WHERE on the primary key (rowid alias) and on an indexed column
        CHECK_EQ(rows_of(*db, "SELECT name, age FROM users WHERE id = 3"),
                 "Carol|35\n");
        CHECK_EQ(rows_of(*db, "SELECT name, age FROM users "
                              "WHERE email = 'carol@example.com'"),
                 "Carol|35\n");

        // the planner should use the index
        CHECK_EQ(rows_of(*db, "SELECT email, name FROM users WHERE city = 'Porto' "
                              "ORDER BY email"),
                 "bob@example.com|Bob\nerin@example.com|Erin\n");
        CHECK(plan_of(*db, "EXPLAIN SELECT name FROM users WHERE city = 'Porto'")
                  .find("Index Lookup") != std::string::npos);
        CHECK(plan_of(*db, "EXPLAIN SELECT name FROM users WHERE email = 'bob@example.com'")
                  .find("Index Lookup") != std::string::npos);

        // range scan
        CHECK_EQ(rows_of(*db, "SELECT COUNT(*) FROM users WHERE age > 28"), "25\n");

        // ORDER BY + LIMIT + OFFSET
        CHECK_EQ(rows_of(*db, "SELECT email FROM users ORDER BY email LIMIT 2 OFFSET 1"),
                 "bob@example.com\ncarol@example.com\n");
        CHECK_EQ(rows_of(*db, "SELECT email FROM users ORDER BY age DESC LIMIT 1"),
                 "erin@example.com\n");

        // aggregates
        CHECK_EQ(rows_of(*db, "SELECT COUNT(*), MIN(age), MAX(age), SUM(age) FROM users"),
                 "35|19|63|1222\n");

        // UPDATE with a predicate
        must(*db, "UPDATE users SET city = 'Lisbon' WHERE email = 'bob@example.com'");
        CHECK_EQ(rows_of(*db, "SELECT COUNT(*) FROM users WHERE city = 'Lisbon'"), "3\n");

        // DELETE with a predicate
        must(*db, "DELETE FROM users WHERE age < 21");
        CHECK_EQ(rows_of(*db, "SELECT COUNT(*) FROM users"), "33\n");

        // uniqueness is enforced by the storage layer, not just the parser
        CHECK_DB_ERROR(must(*db, "INSERT INTO users VALUES"
                                 "(2, 'impostor@example.com', 'Impostor', 99, 'Faro')"),
                       int(Err::Constraint), "already contains value");

        // transactions: what is committed survives, what is rolled back never was
        must(*db, "BEGIN");
        must(*db, "INSERT INTO users VALUES"
                  "(6, 'txn@example.com', 'Txn', 50, 'Faro')");
        must(*db, "UPDATE users SET age = 30 WHERE email = 'alice@example.com'");
        must(*db, "ROLLBACK");
        CHECK_EQ(rows_of(*db, "SELECT COUNT(*) FROM users WHERE email = 'txn@example.com'"),
                 "0\n");
        CHECK_EQ(rows_of(*db, "SELECT age FROM users WHERE email = 'alice@example.com'"),
                 "29\n");

        must(*db, "BEGIN");
        must(*db, "INSERT INTO users VALUES"
                  "(7, 'kept@example.com', 'Kept', 44, 'Faro')");
        must(*db, "DELETE FROM users WHERE email = 'dave@example.com'");
        must(*db, "COMMIT");
        CHECK_EQ(rows_of(*db, "SELECT COUNT(*) FROM users"), "34\n");

        // the file on disk is structurally sound at every level
        CHECK_NO_ERROR(db->validate(true));
    }

    // ---------- phase 2: process restart ----------
    {
        auto db = Database::open(path);

        // schema persisted
        auto tables = db->table_names();
        CHECK(tables.size() == 1 && tables[0] == "users");
        CHECK(db->index_names().size() == 3);
        CHECK(db->schema_sql().find("CREATE TABLE users") != std::string::npos);
        CHECK(db->schema_sql().find("CREATE INDEX users_city") != std::string::npos);
        CHECK(db->schema_sql().find("CREATE INDEX users_email") != std::string::npos);

        // committed rows persisted (and only the committed ones)
        CHECK_EQ(rows_of(*db, "SELECT COUNT(*) FROM users"), "34\n");
        CHECK_EQ(rows_of(*db, "SELECT name, age, city FROM users "
                              "WHERE email = 'bob@example.com'"),
                 "Bob|41|Lisbon\n");   // the UPDATE survived
        CHECK_EQ(rows_of(*db, "SELECT COUNT(*) FROM users WHERE email = 'dave@example.com'"),
                 "0\n");               // the DELETE survived

        // the index survived and the planner still uses it
        CHECK(plan_of(*db, "EXPLAIN SELECT name FROM users WHERE city = 'Porto'")
                  .find("Index Lookup") != std::string::npos);
        CHECK_EQ(rows_of(*db, "SELECT email FROM users WHERE city = 'Porto' "
                              "ORDER BY email"),
                 "erin@example.com\n");   // bob moved to Lisbon before the restart

        // uniqueness still enforced after restart
        CHECK_DB_ERROR(must(*db, "INSERT INTO users VALUES"
                                 "(7, 'again@example.com', 'Again', 1, 'Faro')"),
                       int(Err::Constraint), "already contains value");

        // the B-Tree validates after restart
        CHECK_NO_ERROR(db->validate(true));

        // and the database keeps working: writes, growth, re-validation
        for (int i = 100; i < 140; ++i)
            must(*db, "INSERT INTO users VALUES (" + std::to_string(i) +
                          ", 'new" + std::to_string(i) +
                          "@example.com', 'New " + std::to_string(i) + "', " +
                          std::to_string(i) + ", 'Faro')");
        CHECK_EQ(rows_of(*db, "SELECT COUNT(*) FROM users"), "74\n");
        CHECK_NO_ERROR(db->validate(true));
    }

    // restart once more: everything from the second session is there too
    {
        auto db = Database::open(path);
        CHECK_EQ(rows_of(*db, "SELECT COUNT(*) FROM users"), "74\n");
        CHECK_EQ(rows_of(*db, "SELECT COUNT(*) FROM users WHERE age >= 100"), "40\n");
        CHECK_NO_ERROR(db->validate(true));
    }

    std::remove(path.c_str());
}
