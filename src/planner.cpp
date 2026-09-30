#include "planner.hpp"

#include "util.hpp"

namespace sc {

namespace {

void collect_conjuncts(const Expr* e, std::vector<const Expr*>& out) {
    if (!e) return;
    if (e->kind == Expr::Kind::Binary && e->bop == BinOp::And) {
        collect_conjuncts(e->lhs.get(), out);
        collect_conjuncts(e->rhs.get(), out);
    } else {
        out.push_back(e);
    }
}

// A conjunct is index-usable when one side is a plain column reference and
// the other side is a literal (no columns, no aggregates).
bool is_usable_column(const Expr* e) {
    return e && e->kind == Expr::Kind::Column;
}
bool is_usable_literal(const Expr* e) {
    return e && e->kind == Expr::Kind::Literal;
}

BinOp flip(BinOp op) {
    switch (op) {
        case BinOp::Lt: return BinOp::Gt;
        case BinOp::Le: return BinOp::Ge;
        case BinOp::Gt: return BinOp::Lt;
        case BinOp::Ge: return BinOp::Le;
        default: return op;
    }
}

} // namespace

Plan plan_query(const Catalog& catalog, const TableDef& table, const Expr* where) {
    Plan plan;
    plan.table = &table;
    plan.filter = where;

    std::vector<const Expr*> conjuncts;
    collect_conjuncts(where, conjuncts);

    // Preference order: rowid equality > index equality > index range.
    const Expr* rowid_eq = nullptr;
    const Expr* index_eq = nullptr;
    const IndexDef* index_eq_def = nullptr;
    const Expr* range = nullptr;
    const IndexDef* range_def = nullptr;
    BinOp range_op = BinOp::Eq;

    for (const Expr* c : conjuncts) {
        if (c->kind != Expr::Kind::Binary) continue;
        BinOp op = c->bop;
        const Expr *col = nullptr, *lit = nullptr;
        if (is_usable_column(c->lhs.get()) && is_usable_literal(c->rhs.get())) {
            col = c->lhs.get();
            lit = c->rhs.get();
        } else if (is_usable_column(c->rhs.get()) && is_usable_literal(c->lhs.get())) {
            col = c->rhs.get();
            lit = c->lhs.get();
            op = flip(op);
        } else {
            continue;
        }

        const Value& v = lit->lit;
        int idx = table.column_index(col->column);
        bool is_rowid = idx < 0 && lower(col->column) == "rowid";
        if (idx < 0 && !is_rowid) continue;  // unknown column: executor reports
        if (is_rowid || (table.ipk_index == idx)) is_rowid = true;
        if (idx < 0) idx = table.ipk_index;  // rowid alias column

        if (is_rowid) {
            if (op == BinOp::Eq && v.is_int() && !rowid_eq) rowid_eq = c;
            continue;
        }

        const IndexDef* def = nullptr;
        for (const IndexDef* ix : catalog.indexes_on_table(table.name))
            if (ix->column == col->column ||
                table.column_index(ix->column) == idx) {
                def = ix;
                break;
            }
        if (!def) continue;

        if (op == BinOp::Eq && !v.is_null()) {
            if (!index_eq) {
                index_eq = c;
                index_eq_def = def;
            }
        } else if ((op == BinOp::Lt || op == BinOp::Le || op == BinOp::Gt ||
                    op == BinOp::Ge) &&
                   !v.is_null()) {
            if (!range) {
                range = c;
                range_def = def;
                range_op = op;
            }
        }
    }

    if (rowid_eq) {
        const Expr *col = rowid_eq->lhs.get(), *lit = rowid_eq->rhs.get();
        if (!is_usable_column(col) || !is_usable_literal(lit)) {
            col = rowid_eq->rhs.get();
            lit = rowid_eq->lhs.get();
        }
        plan.type = Plan::Type::RowidSeek;
        plan.rowid = lit->lit.as_int();
        return plan;
    }
    if (index_eq) {
        const Expr *col = index_eq->lhs.get(), *lit = index_eq->rhs.get();
        if (!is_usable_column(col) || !is_usable_literal(lit)) {
            col = index_eq->rhs.get();
            lit = index_eq->lhs.get();
        }
        plan.type = Plan::Type::IndexSeek;
        plan.index = index_eq_def;
        plan.value = lit->lit;
        return plan;
    }
    if (range) {
        const Expr *col = range->lhs.get(), *lit = range->rhs.get();
        if (!is_usable_column(col) || !is_usable_literal(lit)) {
            col = range->rhs.get();
            lit = range->lhs.get();
        }
        plan.type = Plan::Type::IndexRange;
        plan.index = range_def;
        plan.value = lit->lit;
        switch (range_op) {
            case BinOp::Gt: plan.has_low = true; plan.low_inclusive = false; break;
            case BinOp::Ge: plan.has_low = true; plan.low_inclusive = true; break;
            case BinOp::Lt: plan.has_high = true; plan.high_inclusive = false; break;
            case BinOp::Le: plan.has_high = true; plan.high_inclusive = true; break;
            default: break;
        }
        return plan;
    }
    return plan;
}

std::string Plan::describe() const {
    std::string pred = filter ? filter->describe() : std::string("(none)");
    switch (type) {
        case Plan::Type::SeqScan:
            return "Table Scan\n  Table: " + table->name + "\n  Predicate: " + pred;
        case Plan::Type::RowidSeek:
            return "Index Lookup\n  Index: " + table->name + "." +
                   (table->ipk_index >= 0 ? table->columns[size_t(table->ipk_index)].name
                                          : std::string("rowid")) +
                   " (primary key)\n  Table: " + table->name + "\n  Predicate: " + pred;
        case Plan::Type::IndexSeek:
            return "Index Lookup\n  Index: " + index->name + "\n  Table: " + table->name +
                   "\n  Predicate: " + pred;
        case Plan::Type::IndexRange: {
            std::string op = has_low ? (low_inclusive ? ">=" : ">") : (high_inclusive ? "<=" : "<");
            return "Index Scan\n  Index: " + index->name + "\n  Table: " + table->name +
                   "\n  Range: " + index->column + " " + op + " " + value.sql_literal() +
                   "\n  Predicate: " + pred;
        }
    }
    return "?";
}

} // namespace sc
