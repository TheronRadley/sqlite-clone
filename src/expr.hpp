// expr.hpp — expression evaluation with SQL three-valued logic (3VL).
//
// NULL semantics (documented in ARCHITECTURE.md, matching SQLite):
//   * Any comparison involving NULL yields NULL.
//   * AND: false if any operand is false, NULL if any operand is NULL,
//     else true.
//   * OR: true if any operand is true, NULL if any operand is NULL, else
//     false.
//   * NOT NULL is NULL.
//   * Arithmetic with NULL is NULL. Integer overflow and non-integer
//     operands raise TypeError. Division or modulo by zero yields NULL
//     (SQLite behavior).
//   * IS NULL / IS NOT NULL never return NULL.
//   * WHERE keeps a row only when the predicate evaluates to true
//     (integers: nonzero = true). TEXT in a boolean position is a
//     TypeError.
#pragma once

#include "ast.hpp"
#include "schema.hpp"
#include "value.hpp"

namespace sc {

struct RowContext {
    const TableDef* table = nullptr;   // null for FROM-less queries
    int64_t rowid = 0;
    const std::vector<Value>* values = nullptr;
};

// Evaluate an expression against a row (ctx == nullptr for FROM-less).
// Throws ExecutionError for unknown columns, TypeError for type misuse.
Value eval_expr(const Expr& e, const RowContext* ctx);

// SQL truthiness of a predicate result; throws TypeError on TEXT.
bool is_true(const Value& v);

// True when the expression contains an aggregate function call.
bool contains_aggregate(const Expr& e);

// Verify that every column reference in `e` resolves against `table`
// (including the implicit `rowid`). Called before scanning so that
// "SELECT nosuch FROM empty_table" fails like it would on a non-empty
// table instead of silently returning zero rows.
void validate_columns(const Expr& e, const TableDef& table);

} // namespace sc
