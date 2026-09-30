#include "expr.hpp"

#include "util.hpp"

namespace sc {

namespace {

Value bool_value(bool b) { return Value::integer(b ? 1 : 0); }

// tri-state: 0 = false, 1 = true, 2 = null
int tri(const Value& v) {
    if (v.is_null()) return 2;
    if (v.is_int()) return v.as_int() != 0;
    throw DbError::type(str("expected a boolean condition, found TEXT value '",
                            v.display(), "'"));
}

Value arith(BinOp op, const Value& l, const Value& r) {
    if (l.is_null() || r.is_null()) return Value::null();
    if (!l.is_int() || !r.is_int())
        throw DbError::type(str("arithmetic requires INTEGER operands, found ",
                                l.type_name(), " and ", r.type_name()));
    int64_t a = l.as_int(), b = r.as_int(), out = 0;
    switch (op) {
        case BinOp::Add:
            if (add_ovf(a, b, &out))
                throw DbError::type(str("integer overflow in ", a, " + ", b));
            return Value::integer(out);
        case BinOp::Sub:
            if (sub_ovf(a, b, &out))
                throw DbError::type(str("integer overflow in ", a, " - ", b));
            return Value::integer(out);
        case BinOp::Mul:
            if (mul_ovf(a, b, &out))
                throw DbError::type(str("integer overflow in ", a, " * ", b));
            return Value::integer(out);
        case BinOp::Div:
            if (b == 0) return Value::null();  // SQL: division by zero is NULL
            if (a == INT64_MIN && b == -1)
                throw DbError::type(str("integer overflow in ", a, " / ", b));
            return Value::integer(a / b);
        case BinOp::Mod:
            if (b == 0) return Value::null();
            if (a == INT64_MIN && b == -1) return Value::integer(0);
            return Value::integer(a % b);
        default:
            throw DbError::exec("internal error: not an arithmetic operator");
    }
}

} // namespace

bool is_true(const Value& v) { return tri(v) == 1; }

Value eval_expr(const Expr& e, const RowContext* ctx) {
    switch (e.kind) {
        case Expr::Kind::Literal:
            return e.lit;

        case Expr::Kind::Column: {
            if (!ctx || !ctx->table)
                throw DbError::exec(str("no such column: ", e.column, " (no FROM clause)"));
            int idx = ctx->table->column_index(e.column);
            if (idx < 0) {
                if (lower(e.column) == "rowid")
                    return Value::integer(ctx->rowid);
                throw DbError::exec(str("no such column: ", e.column, " (table '",
                                        ctx->table->name, "')"));
            }
            const Value& v = (*ctx->values)[size_t(idx)];
            // INTEGER PRIMARY KEY columns are rowid aliases
            if (ctx->table->ipk_index == idx)
                return Value::integer(ctx->rowid);
            return v;
        }

        case Expr::Kind::Binary: {
            switch (e.bop) {
                case BinOp::And: {
                    int a = tri(eval_expr(*e.lhs, ctx));
                    if (a == 0) return bool_value(false);
                    int b = tri(eval_expr(*e.rhs, ctx));
                    if (b == 0) return bool_value(false);
                    if (a == 2 || b == 2) return Value::null();
                    return bool_value(true);
                }
                case BinOp::Or: {
                    int a = tri(eval_expr(*e.lhs, ctx));
                    if (a == 1) return bool_value(true);
                    int b = tri(eval_expr(*e.rhs, ctx));
                    if (b == 1) return bool_value(true);
                    if (a == 2 || b == 2) return Value::null();
                    return bool_value(false);
                }
                case BinOp::Eq: case BinOp::Ne: case BinOp::Lt:
                case BinOp::Le: case BinOp::Gt: case BinOp::Ge: {
                    Value l = eval_expr(*e.lhs, ctx);
                    Value r = eval_expr(*e.rhs, ctx);
                    if (l.is_null() || r.is_null()) return Value::null();
                    int c = l.compare(r);
                    switch (e.bop) {
                        case BinOp::Eq: return bool_value(c == 0);
                        case BinOp::Ne: return bool_value(c != 0);
                        case BinOp::Lt: return bool_value(c < 0);
                        case BinOp::Le: return bool_value(c <= 0);
                        case BinOp::Gt: return bool_value(c > 0);
                        case BinOp::Ge: return bool_value(c >= 0);
                        default: break;
                    }
                    break;
                }
                case BinOp::Add: case BinOp::Sub: case BinOp::Mul:
                case BinOp::Div: case BinOp::Mod:
                    return arith(e.bop, eval_expr(*e.lhs, ctx),
                                 eval_expr(*e.rhs, ctx));
            }
            break;
        }

        case Expr::Kind::Unary: {
            switch (e.uop) {
                case UnOp::Not: {
                    Value v = eval_expr(*e.child, ctx);
                    int t = tri(v);
                    if (t == 2) return Value::null();
                    return bool_value(t == 0);
                }
                case UnOp::Neg: {
                    Value v = eval_expr(*e.child, ctx);
                    if (v.is_null()) return Value::null();
                    if (!v.is_int())
                        throw DbError::type(str("cannot negate ", v.type_name(),
                                                " value '", v.display(), "'"));
                    if (v.as_int() == INT64_MIN)
                        throw DbError::type("integer overflow in -(-9223372036854775808)");
                    return Value::integer(-v.as_int());
                }
                case UnOp::IsNull: {
                    Value v = eval_expr(*e.child, ctx);
                    return bool_value(v.is_null());
                }
                case UnOp::IsNotNull: {
                    Value v = eval_expr(*e.child, ctx);
                    return bool_value(!v.is_null());
                }
            }
            break;
        }

        case Expr::Kind::Agg:
            throw DbError::exec("aggregate functions are only allowed in the SELECT list");
    }
    throw DbError::exec("internal error: unknown expression kind");
}

bool contains_aggregate(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::Agg: return true;
        case Expr::Kind::Binary:
            return contains_aggregate(*e.lhs) || contains_aggregate(*e.rhs);
        case Expr::Kind::Unary: return contains_aggregate(*e.child);
        default: return false;
    }
}


// ---- static column validation (see header) ----
void validate_columns(const Expr& e, const TableDef& table) {
    switch (e.kind) {
        case Expr::Kind::Column: {
            int idx = table.column_index(e.column);
            if (idx < 0 && lower(e.column) != "rowid")
                throw DbError::exec(str("no such column: ", e.column, " (table '",
                                        table.name, "')"));
            return;
        }
        case Expr::Kind::Literal:
            return;
        case Expr::Kind::Unary:
            validate_columns(*e.child, table);
            return;
        case Expr::Kind::Binary:
            validate_columns(*e.lhs, table);
            validate_columns(*e.rhs, table);
            return;
        case Expr::Kind::Agg:
            if (e.child) validate_columns(*e.child, table);
            return;
    }
    throw DbError::exec("internal error: unknown expression kind");
}

} // namespace sc
