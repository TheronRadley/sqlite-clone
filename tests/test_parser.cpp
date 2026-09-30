// test_parser.cpp — parser and AST shape tests.
#include "parser.hpp"
#include "test_util.hpp"

using namespace sc;

namespace {

const Statement& one(const std::string& sql) {
    static thread_local std::vector<std::unique_ptr<Statement>> holder;
    holder = parse(sql);
    CHECK_EQ(holder.size(), size_t(1));
    return *holder[0];
}

} // namespace

TEST(parser_select_star) {
    const Statement& s = one("SELECT * FROM users;");
    CHECK(s.kind == Statement::Kind::Select);
    CHECK(s.select.star);
    CHECK(s.select.has_table);
    CHECK_EQ(s.select.table, std::string("users"));
    CHECK(!s.select.where);
    CHECK(s.select.order_by.empty());
}

TEST(parser_select_full) {
    const Statement& s = one(
        "SELECT name, age FROM users WHERE age >= 18 AND name = 'Ron' "
        "ORDER BY age DESC, name LIMIT 10 OFFSET 5;");
    CHECK(s.kind == Statement::Kind::Select);
    CHECK(!s.select.star);
    CHECK_EQ(s.select.columns.size(), size_t(2));
    CHECK_EQ(s.select.columns[0]->column, std::string("name"));
    CHECK_EQ(s.select.columns[1]->column, std::string("age"));
    CHECK_EQ(s.select.order_by.size(), size_t(2));
    CHECK(s.select.order_by[0].desc);    // age DESC
    CHECK(!s.select.order_by[1].desc);   // name (ascending)
    CHECK(s.select.limit->kind == Expr::Kind::Literal);
    CHECK_EQ(s.select.limit->lit.as_int(), int64_t(10));
    CHECK_EQ(s.select.offset->lit.as_int(), int64_t(5));
}

TEST(parser_select_without_from) {
    const Statement& s = one("SELECT 1;");
    CHECK(s.kind == Statement::Kind::Select);
    CHECK(!s.select.has_table);
    CHECK_EQ(s.select.columns.size(), size_t(1));
    CHECK(s.select.columns[0]->kind == Expr::Kind::Literal);
    CHECK_EQ(s.select.columns[0]->lit.as_int(), int64_t(1));
}

// WHERE age > 18 AND name = 'Ron' must NOT parse as age > (18 AND name)
TEST(parser_operator_precedence_and_vs_comparison) {
    const Statement& s = one("SELECT x FROM t WHERE age > 18 AND name = 'Ron';");
    const Expr& w = *s.select.where;
    CHECK(w.kind == Expr::Kind::Binary);
    CHECK(w.bop == BinOp::And);
    CHECK(w.lhs->kind == Expr::Kind::Binary);
    CHECK(w.lhs->bop == BinOp::Gt);           // comparison binds tighter than AND
    CHECK(w.lhs->lhs->column == std::string("age"));
    CHECK(w.lhs->rhs->lit.as_int() == int64_t(18));
    CHECK(w.rhs->bop == BinOp::Eq);
    CHECK_EQ(w.rhs->rhs->lit.as_text(), std::string("Ron"));
}

TEST(parser_operator_precedence_arithmetic) {
    const Statement& s = one("SELECT 1 + 2 * 3 - 4 FROM t;");
    const Expr& e = *s.select.columns[0];
    // 1 + 2*3 - 4 == (1 + (2*3)) - 4
    CHECK(e.kind == Expr::Kind::Binary);
    CHECK(e.bop == BinOp::Sub);
    CHECK(e.lhs->bop == BinOp::Add);
    CHECK(e.rhs->lit.as_int() == int64_t(4));
    CHECK(e.lhs->rhs->bop == BinOp::Mul);
}

TEST(parser_operator_precedence_or_not) {
    const Statement& s = one("SELECT x FROM t WHERE a = 1 OR NOT b = 2 AND c = 3;");
    const Expr& w = *s.select.where;
    CHECK(w.bop == BinOp::Or);
    CHECK(w.rhs->bop == BinOp::And);           // AND binds tighter than OR
    CHECK(w.rhs->lhs->kind == Expr::Kind::Unary);
    CHECK(w.rhs->lhs->uop == UnOp::Not);
}

TEST(parser_unary_minus_and_parens) {
    const Statement& s = one("SELECT -(1 + 2) FROM t;");
    const Expr& e = *s.select.columns[0];
    CHECK(e.kind == Expr::Kind::Unary);
    CHECK(e.uop == UnOp::Neg);
    CHECK(e.child->kind == Expr::Kind::Binary);
    CHECK(e.child->bop == BinOp::Add);
}

TEST(parser_is_null) {
    const Statement& s = one("SELECT a FROM t WHERE a IS NULL AND b IS NOT NULL;");
    const Expr& w = *s.select.where;
    CHECK(w.bop == BinOp::And);
    CHECK(w.lhs->kind == Expr::Kind::Unary);
    CHECK(w.lhs->uop == UnOp::IsNull);
    CHECK(w.rhs->uop == UnOp::IsNotNull);
}

TEST(parser_create_table) {
    const Statement& s = one(
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER);");
    CHECK(s.kind == Statement::Kind::CreateTable);
    CHECK_EQ(s.create_table.table, std::string("users"));
    CHECK_EQ(s.create_table.columns.size(), size_t(3));
    CHECK_EQ(s.create_table.columns[0].name, std::string("id"));
    CHECK(s.create_table.columns[0].primary_key);
    CHECK(s.create_table.columns[0].type == ColType::Integer);
    CHECK(s.create_table.columns[1].type == ColType::Text);
    CHECK(!s.create_table.columns[2].primary_key);
}

TEST(parser_create_index) {
    const Statement& s = one("CREATE INDEX users_age_idx ON users(age);");
    CHECK(s.kind == Statement::Kind::CreateIndex);
    CHECK_EQ(s.create_index.index, std::string("users_age_idx"));
    CHECK_EQ(s.create_index.table, std::string("users"));
    CHECK_EQ(s.create_index.column, std::string("age"));
}

TEST(parser_insert) {
    const Statement& s = one(
        "INSERT INTO users VALUES (1, 'Alice', 30), (2, 'Bob', NULL);");
    CHECK(s.kind == Statement::Kind::Insert);
    CHECK_EQ(s.insert.table, std::string("users"));
    CHECK(!s.insert.has_columns);
    CHECK_EQ(s.insert.rows.size(), size_t(2));
    CHECK_EQ(s.insert.rows[0].size(), size_t(3));
    CHECK(s.insert.rows[1][2]->kind == Expr::Kind::Literal);
    CHECK(s.insert.rows[1][2]->lit.is_null());
}

TEST(parser_insert_column_list) {
    const Statement& s = one("INSERT INTO users (name, age) VALUES ('Eve', 9);");
    CHECK(s.insert.has_columns);
    CHECK_EQ(s.insert.columns.size(), size_t(2));
    CHECK_EQ(s.insert.columns[0], std::string("name"));
    CHECK_EQ(s.insert.rows.size(), size_t(1));
}

TEST(parser_update_delete) {
    const Statement& u = one("UPDATE users SET age = 25, name = 'x' WHERE id = 2;");
    CHECK(u.kind == Statement::Kind::Update);
    CHECK_EQ(u.update.sets.size(), size_t(2));
    CHECK_EQ(u.update.sets[0].first, std::string("age"));
    CHECK(u.update.where->bop == BinOp::Eq);

    const Statement& d = one("DELETE FROM users WHERE id = 3;");
    CHECK(d.kind == Statement::Kind::Delete);
    CHECK_EQ(d.del.table, std::string("users"));
    CHECK(d.del.where);

    const Statement& d2 = one("DELETE FROM users;");
    CHECK(!d2.del.where);
}

TEST(parser_transactions_and_explain) {
    CHECK(one("BEGIN;").kind == Statement::Kind::Begin);
    CHECK(one("BEGIN TRANSACTION;").kind == Statement::Kind::Begin);
    CHECK(one("COMMIT;").kind == Statement::Kind::Commit);
    CHECK(one("COMMIT TRANSACTION;").kind == Statement::Kind::Commit);
    CHECK(one("ROLLBACK;").kind == Statement::Kind::Rollback);

    const Statement& e = one("EXPLAIN SELECT * FROM t;");
    CHECK(e.kind == Statement::Kind::Explain);
    CHECK(e.inner->kind == Statement::Kind::Select);
}

TEST(parser_multiple_statements) {
    auto stmts = parse("SELECT 1; SELECT 2;; SELECT 3;");
    CHECK_EQ(stmts.size(), size_t(3));
    auto empty = parse(";;;");
    CHECK_EQ(empty.size(), size_t(0));
}

TEST(parser_aggregates) {
    const Statement& s = one("SELECT COUNT(*), COUNT(age), MIN(age), MAX(age), SUM(age) FROM t;");
    CHECK_EQ(s.select.columns.size(), size_t(5));
    CHECK(s.select.columns[0]->kind == Expr::Kind::Agg);
    CHECK(s.select.columns[0]->agg == AggFn::Count);
    CHECK(s.select.columns[0]->star);
    CHECK(!s.select.columns[1]->star);
    CHECK(s.select.columns[1]->child->column == std::string("age"));
    CHECK(s.select.columns[2]->agg == AggFn::Min);
    CHECK(s.select.columns[4]->agg == AggFn::Sum);
}

TEST(parser_errors_identify_location) {
    CHECK_DB_ERROR(parse("SELECT FROM t;"), int(Err::Parse), "parse error at line 1, column 8");
    CHECK_DB_ERROR(parse("SELECT * FROM t WHERE;"), int(Err::Parse), "column 22");
    CHECK_DB_ERROR(parse("CREATE TABLE t (a);"), int(Err::Parse), "column 18");
    CHECK_DB_ERROR(parse("SELECT * FROM t WHERE a = ;"), int(Err::Parse),
                   "column 27");
    CHECK_DB_ERROR(parse("INSERT INTO t VALUES (1,);"), int(Err::Parse),
                   "column 25");
    CHECK_DB_ERROR(parse("SELECT * t;"), int(Err::Parse), "expected");
    CHECK_DB_ERROR(parse("FROBNICATE;"), int(Err::Parse), "a SQL statement");
    CHECK_DB_ERROR(parse("SELECT 999999999999999999999999;"), int(Err::Lex), "out of range");
    CHECK_DB_ERROR(parse("SELECT * FROM t LIMIT 1 OFFSET 2 OFFSET 3;"), int(Err::Parse),
                   "end of statement");
}

TEST(parser_offset_requires_limit) {
    CHECK_DB_ERROR(parse("SELECT * FROM t OFFSET 2;"), int(Err::Parse),
                   "'LIMIT' before OFFSET");
}
