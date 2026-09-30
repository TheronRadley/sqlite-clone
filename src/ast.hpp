// ast.hpp — Abstract Syntax Tree for the supported SQL subset.
//
// Pure data: no behavior beyond construction, cloning, and rendering.
// The parser produces it; the planner/executor consume it; tests clone it.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "value.hpp"

namespace sc {

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

enum class BinOp { And, Or, Eq, Ne, Lt, Le, Gt, Ge, Add, Sub, Mul, Div, Mod };
enum class UnOp { Not, Neg, IsNull, IsNotNull };
enum class AggFn { Count, Min, Max, Sum };

const char* binop_name(BinOp op);

struct Expr {
    enum class Kind { Literal, Column, Binary, Unary, Agg };
    Kind kind;

    // Literal
    Value lit;
    // Column
    std::string column;
    int line = 0, col = 0;      // for "no such column" messages
    // Binary
    BinOp bop{};
    ExprPtr lhs, rhs;
    // Unary
    UnOp uop{};
    ExprPtr child;
    // Aggregate
    AggFn agg{};                // valid when kind == Agg
    bool star = false;          // COUNT(*)

    static ExprPtr literal(Value v) {
        auto e = std::make_unique<Expr>();
        e->kind = Kind::Literal;
        e->lit = std::move(v);
        return e;
    }
    static ExprPtr col_ref(std::string name, int line, int col) {
        auto e = std::make_unique<Expr>();
        e->kind = Kind::Column;
        e->column = std::move(name);
        e->line = line;
        e->col = col;
        return e;
    }
    static ExprPtr binary(BinOp op, ExprPtr l, ExprPtr r) {
        auto e = std::make_unique<Expr>();
        e->kind = Kind::Binary;
        e->bop = op;
        e->lhs = std::move(l);
        e->rhs = std::move(r);
        return e;
    }
    static ExprPtr unary(UnOp op, ExprPtr c) {
        auto e = std::make_unique<Expr>();
        e->kind = Kind::Unary;
        e->uop = op;
        e->child = std::move(c);
        return e;
    }
    static ExprPtr agg_expr(AggFn fn, bool star, ExprPtr arg) {
        auto e = std::make_unique<Expr>();
        e->kind = Kind::Agg;
        e->agg = fn;
        e->star = star;
        e->child = std::move(arg);
        return e;
    }

    ExprPtr clone() const {
        auto e = std::make_unique<Expr>();
        e->kind = kind;
        e->lit = lit;
        e->column = column;
        e->line = line;
        e->col = col;
        e->bop = bop;
        e->uop = uop;
        e->agg = agg;
        e->star = star;
        if (lhs) e->lhs = lhs->clone();
        if (rhs) e->rhs = rhs->clone();
        if (child) e->child = child->clone();
        return e;
    }

    // Render back to SQL-ish text (used for EXPLAIN and column labels).
    std::string describe() const;
};

struct OrderTerm {
    ExprPtr expr;
    bool desc = false;
};

struct SelectStmt {
    bool star = false;                 // SELECT *
    std::vector<ExprPtr> columns;      // projection expressions
    bool has_table = false;
    std::string table;
    ExprPtr where;                     // may be null
    std::vector<OrderTerm> order_by;
    ExprPtr limit, offset;             // may be null
};

struct InsertStmt {
    std::string table;
    bool has_columns = false;
    std::vector<std::string> columns;  // optional explicit column list
    std::vector<std::vector<ExprPtr>> rows;
};

struct UpdateStmt {
    std::string table;
    std::vector<std::pair<std::string, ExprPtr>> sets;
    ExprPtr where;
};

struct DeleteStmt {
    std::string table;
    ExprPtr where;
};

struct ColumnDefAst {
    std::string name;
    ColType type;
    bool primary_key = false;
};

struct CreateTableStmt {
    std::string table;
    std::vector<ColumnDefAst> columns;
};

struct CreateIndexStmt {
    std::string index;
    std::string table;
    std::string column;
};

struct Statement {
    enum class Kind {
        Select, Insert, Update, Delete, CreateTable, CreateIndex,
        Begin, Commit, Rollback, Explain,
    };
    Kind kind;

    SelectStmt select;
    InsertStmt insert;
    UpdateStmt update;
    DeleteStmt del;
    CreateTableStmt create_table;
    CreateIndexStmt create_index;
    std::unique_ptr<Statement> inner;  // for Explain

    explicit Statement(Kind k) : kind(k) {}
};

} // namespace sc
