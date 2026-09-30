// executor.cpp — statement execution: the bridge from AST to B-Trees.
#include "executor.hpp"

#include <algorithm>

#include "expr.hpp"
#include "planner.hpp"
#include "util.hpp"

namespace sc {

namespace {

constexpr size_t MAX_COLUMNS = 512;

// =====================================================================
// Row helpers
// =====================================================================

struct FetchedRow {
    int64_t rowid = 0;
    std::vector<Value> values;   // full column values (IPK substituted)
};

FetchedRow decode_row(const TableDef& table, int64_t rowid,
                      const std::vector<uint8_t>& payload) {
    Record rec = decode_record(payload);
    if (rec.values.size() != table.columns.size())
        throw DbError::storage(str("table '", table.name, "': row ", rowid, " has ",
                                   rec.values.size(), " values but the schema declares ",
                                   table.columns.size(), " columns"));
    if (table.ipk_index >= 0) rec.values[size_t(table.ipk_index)] = Value::integer(rowid);
    return FetchedRow{rowid, std::move(rec.values)};
}

std::vector<FetchedRow> fetch_rows(Pager& pager, const Plan& plan) {
    std::vector<FetchedRow> out;
    const TableDef& table = *plan.table;
    BTree<RowIdKey> ttree(pager, table.root_page);

    switch (plan.type) {
        case Plan::Type::SeqScan: {
            auto cur = ttree.cursor();
            BTree<RowIdKey>::Entry e;
            while (cur.next(e)) out.push_back(decode_row(table, e.key.id, e.payload));
            break;
        }
        case Plan::Type::RowidSeek: {
            std::vector<uint8_t> payload;
            if (ttree.search(RowIdKey(plan.rowid), payload))
                out.push_back(decode_row(table, plan.rowid, payload));
            break;
        }
        case Plan::Type::IndexSeek:
        case Plan::Type::IndexRange: {
            BTree<IndexKey> itree(pager, plan.index->root_page);
            auto cur = itree.cursor();
            if (plan.type == Plan::Type::IndexSeek) {
                cur.seek(IndexKey(plan.value, INT64_MIN), true);
            } else if (plan.has_low) {
                // rowid is the tie-breaker inside one value, so:
                //   >= v  starts at the first entry with value v
                //   >  v  starts past the last entry with value v
                cur.seek(IndexKey(plan.value, plan.low_inclusive ? INT64_MIN : INT64_MAX),
                         plan.low_inclusive);
            }
            BTree<IndexKey>::Entry e;
            while (cur.next(e)) {
                if (plan.type == Plan::Type::IndexSeek &&
                    e.key.value.compare(plan.value) != 0)
                    break;
                if (plan.type == Plan::Type::IndexRange && plan.has_high) {
                    int c = e.key.value.compare(plan.value);
                    if (c > 0 || (c == 0 && !plan.high_inclusive)) break;
                }
                std::vector<uint8_t> payload;
                if (ttree.search(RowIdKey(e.key.rowid), payload))
                    out.push_back(decode_row(table, e.key.rowid, payload));
            }
            break;
        }
    }
    return out;
}

std::vector<FetchedRow> filter_rows(const std::vector<FetchedRow>& rows, const TableDef& table,
                                    const Expr* where) {
    if (!where) return rows;
    std::vector<FetchedRow> out;
    for (const FetchedRow& r : rows) {
        RowContext ctx{&table, r.rowid, &r.values};
        if (is_true(eval_expr(*where, &ctx))) out.push_back(r);
    }
    return out;
}

// =====================================================================
// Index maintenance
// =====================================================================

void index_insert_row(Pager& pager, const Catalog& catalog, const TableDef& table,
                      const std::vector<Value>& values, int64_t rowid) {
    for (const IndexDef* ix : catalog.indexes_on_table(table.name)) {
        int col = table.column_index(ix->column);
        if (col < 0)
            throw DbError::storage(str("index '", ix->name, "' references unknown column '",
                                       ix->column, "'"));
        BTree<IndexKey> itree(pager, ix->root_page);
        itree.insert(IndexKey(values[size_t(col)], rowid), nullptr);
    }
}

void index_remove_row(Pager& pager, const Catalog& catalog, const TableDef& table,
                      const std::vector<Value>& values, int64_t rowid) {
    for (const IndexDef* ix : catalog.indexes_on_table(table.name)) {
        int col = table.column_index(ix->column);
        if (col < 0)
            throw DbError::storage(str("index '", ix->name, "' references unknown column '",
                                       ix->column, "'"));
        BTree<IndexKey> itree(pager, ix->root_page);
        if (!itree.remove(IndexKey(values[size_t(col)], rowid)))
            throw DbError::storage(str("index/table inconsistency: index '", ix->name,
                                       "' is missing the entry for row ", rowid,
                                       " (table '", table.name, "')"));
    }
}

// =====================================================================
// CREATE TABLE / CREATE INDEX
// =====================================================================

ExecResult exec_create_table(Pager& pager, Catalog& catalog, const CreateTableStmt& s) {
    if (s.table.empty()) throw DbError::exec("table name must not be empty");
    if (s.columns.empty())
        throw DbError::exec(str("table '", s.table, "' must have at least one column"));
    if (s.columns.size() > MAX_COLUMNS)
        throw DbError::exec(str("table '", s.table, "' exceeds the ", MAX_COLUMNS,
                                "-column limit"));

    TableDef def;
    def.name = s.table;
    int pk_count = 0;
    for (const ColumnDefAst& c : s.columns) {
        if (c.name.empty()) throw DbError::exec("column name must not be empty");
        if (c.primary_key) {
            if (++pk_count > 1)
                throw DbError::constraint(str("table '", s.table,
                                              "' has more than one PRIMARY KEY column"));
            if (c.type != ColType::Integer)
                throw DbError::constraint(
                    str("column '", c.name,
                        "': only INTEGER PRIMARY KEY is supported (TEXT keys would need a "
                        "separate unique index)"));
        }
        ColumnDef cd;
        cd.name = c.name;
        cd.type = c.type;
        cd.primary_key = c.primary_key;
        if (c.primary_key) def.ipk_index = int(def.columns.size());
        def.columns.push_back(std::move(cd));
    }
    for (size_t i = 0; i < def.columns.size(); ++i)
        for (size_t j = i + 1; j < def.columns.size(); ++j)
            if (lower(def.columns[i].name) == lower(def.columns[j].name))
                throw DbError::constraint(str("table '", s.table, "': duplicate column name '",
                                              def.columns[j].name, "'"));
    if (catalog.find_table(def.name) || catalog.find_index(def.name))
        throw DbError::constraint(str("table '", def.name, "' already exists"));

    def.root_page = BTree<RowIdKey>::create(pager);
    def.sql = table_def_sql(def);
    catalog.add_table(std::move(def));
    return ExecResult::ok(str("table '", s.table, "' created"));
}

ExecResult exec_create_index(Pager& pager, Catalog& catalog, const CreateIndexStmt& s) {
    const TableDef* table = catalog.find_table(s.table);
    if (!table)
        throw DbError::exec(str("no such table: ", s.table, " (in CREATE INDEX)"));
    int col = table->column_index(s.column);
    if (col < 0)
        throw DbError::exec(str("no such column: ", s.column, " (table '", s.table, "')"));
    if (table->ipk_index == col)
        throw DbError::exec(str("column '", s.column,
                                "' is the primary key; it is already indexed"));
    if (catalog.find_index(s.index) || catalog.find_table(s.index))
        throw DbError::constraint(str("index '", s.index, "' already exists"));

    IndexDef def;
    def.name = s.index;
    def.table = table->name;
    def.column = table->columns[size_t(col)].name;
    def.root_page = BTree<IndexKey>::create(pager);
    def.sql = index_def_sql(def);

    // Build the index over the existing rows before publishing it.
    BTree<IndexKey> itree(pager, def.root_page);
    BTree<RowIdKey> ttree(pager, table->root_page);
    uint64_t rows = 0;
    auto cur = ttree.cursor();
    BTree<RowIdKey>::Entry e;
    while (cur.next(e)) {
        FetchedRow row = decode_row(*table, e.key.id, e.payload);
        itree.insert(IndexKey(row.values[size_t(col)], e.key.id), nullptr);
        ++rows;
    }

    catalog.add_index(std::move(def));
    return ExecResult::ok(str("index '", s.index, "' created over ", rows, " row(s)"));
}

// =====================================================================
// INSERT
// =====================================================================

ExecResult exec_insert(Pager& pager, Catalog& catalog, const InsertStmt& s) {
    const TableDef* table = catalog.find_table(s.table);
    if (!table) throw DbError::exec(str("no such table: ", s.table));

    // resolve the optional column list
    std::vector<int> col_map;
    if (s.has_columns) {
        for (const std::string& name : s.columns) {
            int idx = table->column_index(name);
            if (idx < 0)
                throw DbError::exec(str("table '", table->name, "' has no column named '",
                                        name, "'"));
            for (int prev : col_map)
                if (prev == idx)
                    throw DbError::exec(str("column '", name, "' appears twice in the "
                                                              "column list"));
            col_map.push_back(idx);
        }
    }

    size_t expected = s.has_columns ? col_map.size() : table->columns.size();

    BTree<RowIdKey> ttree(pager, table->root_page);
    RowIdKey maxk;
    int64_t next_auto = ttree.max_key(maxk) ? maxk.id + 1 : 1;

    uint64_t inserted = 0;
    for (const std::vector<ExprPtr>& row : s.rows) {
        if (row.size() != expected)
            throw DbError::exec(str("table '", table->name, "' expects ", expected,
                                    " values per row, got ", row.size()));

        std::vector<Value> values(table->columns.size(), Value::null());
        for (size_t j = 0; j < row.size(); ++j) {
            int target = s.has_columns ? col_map[j] : int(j);
            Value v = eval_expr(*row[j], nullptr);
            values[size_t(target)] =
                v.apply_affinity(table->columns[size_t(target)].type,
                                 table->columns[size_t(target)].name);
        }

        // row id: explicit INTEGER PRIMARY KEY value, else auto-assign
        int64_t rowid;
        if (table->ipk_index >= 0 && !values[size_t(table->ipk_index)].is_null()) {
            if (!values[size_t(table->ipk_index)].is_int())
                throw DbError::type(str("PRIMARY KEY column '", 
                                        table->columns[size_t(table->ipk_index)].name,
                                        "' requires an INTEGER"));
            rowid = values[size_t(table->ipk_index)].as_int();
        } else {
            rowid = next_auto++;
        }

        // the record stores NULL in the IPK column; the key carries the value
        std::vector<Value> rec = values;
        if (table->ipk_index >= 0) rec[size_t(table->ipk_index)] = Value::null();
        auto payload = encode_record(rec);

        auto res = ttree.insert(RowIdKey(rowid), &payload);
        if (res == BTree<RowIdKey>::InsertResult::Duplicate)
            throw DbError::constraint(str(
                "PRIMARY KEY '", table->name, ".",
                table->ipk_index >= 0 ? table->columns[size_t(table->ipk_index)].name
                                      : std::string("rowid"),
                "' already contains value ", rowid));
        index_insert_row(pager, catalog, *table, values, rowid);
        ++inserted;
    }
    return ExecResult::ok(str(inserted, " row(s) inserted into '", table->name, "'"));
}

// =====================================================================
// SELECT
// =====================================================================

std::string column_label(const Expr& e) {
    if (e.kind == Expr::Kind::Column) return e.column;
    return e.describe();
}

Value eval_aggregate(const Expr& e, const std::vector<FetchedRow>& rows,
                     const TableDef& table) {
    // e.kind == Agg; evaluate over the (already filtered) rows
    switch (e.agg) {
        case AggFn::Count: {
            if (e.star) return Value::integer(int64_t(rows.size()));
            uint64_t n = 0;
            for (const FetchedRow& r : rows) {
                RowContext ctx{&table, r.rowid, &r.values};
                if (!eval_expr(*e.child, &ctx).is_null()) ++n;
            }
            return Value::integer(int64_t(n));
        }
        case AggFn::Min:
        case AggFn::Max: {
            bool have = false;
            Value best;
            for (const FetchedRow& r : rows) {
                RowContext ctx{&table, r.rowid, &r.values};
                Value v = eval_expr(*e.child, &ctx);
                if (v.is_null()) continue;
                if (!have || (e.agg == AggFn::Min ? v.compare(best) < 0
                                                  : v.compare(best) > 0)) {
                    best = std::move(v);
                    have = true;
                }
            }
            return have ? best : Value::null();
        }
        case AggFn::Sum: {
            bool have = false;
            int64_t sum = 0;
            for (const FetchedRow& r : rows) {
                RowContext ctx{&table, r.rowid, &r.values};
                Value v = eval_expr(*e.child, &ctx);
                if (v.is_null()) continue;
                if (!v.is_int())
                    throw DbError::type("SUM requires INTEGER values");
                if (add_ovf(sum, v.as_int(), &sum))
                    throw DbError::type("integer overflow in SUM");
                have = true;
            }
            return have ? Value::integer(sum) : Value::null();
        }
    }
    throw DbError::exec("internal error: unknown aggregate");
}

ExecResult exec_select(Pager& pager, Catalog& catalog, const SelectStmt& s) {
    // ---- FROM-less select: evaluate one row of expressions ----
    if (!s.has_table) {
        if (s.star) throw DbError::exec("SELECT * requires a FROM clause");
        bool agg = false;
        for (const ExprPtr& e : s.columns) agg = agg || contains_aggregate(*e);
        if (agg)
            throw DbError::exec(
                "aggregate functions are not supported without FROM (no rows to aggregate)");
        std::vector<Value> row;
        for (const ExprPtr& e : s.columns) row.push_back(eval_expr(*e, nullptr));
        std::vector<std::string> cols;
        for (const ExprPtr& e : s.columns) cols.push_back(column_label(*e));
        return ExecResult::table(std::move(cols), {std::move(row)});
    }

    const TableDef* table = catalog.find_table(s.table);
    if (!table) throw DbError::exec(str("no such table: ", s.table));

    // column references must resolve even when the table is empty
    if (s.where) validate_columns(*s.where, *table);
    if (!s.star)
        for (const ExprPtr& e : s.columns) validate_columns(*e, *table);
    for (const OrderTerm& t : s.order_by) validate_columns(*t.expr, *table);

    Plan plan = plan_query(catalog, *table, s.where.get());
    std::vector<FetchedRow> rows = filter_rows(fetch_rows(pager, plan), *table,
                                               s.where.get());

    // ---- projection columns ----
    std::vector<std::string> cols;
    if (s.star) {
        for (const ColumnDef& c : table->columns) cols.push_back(c.name);
    } else {
        for (const ExprPtr& e : s.columns) cols.push_back(column_label(*e));
    }

    // ---- aggregate path ----
    bool any_agg = false;
    if (!s.star)
        for (const ExprPtr& e : s.columns) any_agg = any_agg || contains_aggregate(*e);

    if (any_agg) {
        if (s.star) throw DbError::exec("SELECT * cannot be combined with aggregates");
        if (!s.order_by.empty())
            throw DbError::exec("ORDER BY is not supported in aggregate queries");
        std::vector<Value> out;
        for (const ExprPtr& e : s.columns) {
            if (e->kind == Expr::Kind::Agg) {
                if (e->child && contains_aggregate(*e->child))
                    throw DbError::exec("aggregate arguments cannot contain aggregates");
                out.push_back(eval_aggregate(*e, rows, *table));
            } else if (e->kind == Expr::Kind::Literal) {
                out.push_back(e->lit);
            } else {
                throw DbError::exec(str(
                    "mixed aggregate/non-aggregate projection '", e->describe(),
                    "': non-aggregate columns require GROUP BY, which is not supported"));
            }
        }
        return ExecResult::table(std::move(cols), {std::move(out)});
    }

    // ---- ORDER BY (evaluated on full rows, before LIMIT/OFFSET) ----
    if (!s.order_by.empty()) {
        std::vector<std::vector<Value>> keys(rows.size());
        for (size_t i = 0; i < rows.size(); ++i) {
            RowContext ctx{table, rows[i].rowid, &rows[i].values};
            for (const OrderTerm& t : s.order_by)
                keys[i].push_back(eval_expr(*t.expr, &ctx));
        }
        std::vector<size_t> idx(rows.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
            for (size_t t = 0; t < s.order_by.size(); ++t) {
                int c = keys[a][t].compare(keys[b][t]);
                if (c != 0) return s.order_by[t].desc ? c > 0 : c < 0;
            }
            return false;
        });
        std::vector<FetchedRow> sorted;
        sorted.reserve(rows.size());
        for (size_t i : idx) sorted.push_back(std::move(rows[i]));
        rows = std::move(sorted);
    }

    // ---- LIMIT / OFFSET ----
    auto eval_const = [](const Expr* e, int64_t fallback) -> int64_t {
        if (!e) return fallback;
        Value v = eval_expr(*e, nullptr);
        if (v.is_null()) return fallback;
        if (!v.is_int())
            throw DbError::type(str("LIMIT/OFFSET must be INTEGER, found ", v.type_name()));
        return v.as_int();
    };
    int64_t offset = eval_const(s.offset.get(), 0);
    if (offset < 0) offset = 0;
    int64_t limit = eval_const(s.limit.get(), -1);  // -1 = unlimited

    size_t begin = size_t(std::min<int64_t>(offset, int64_t(rows.size())));
    size_t end = rows.size();
    if (limit >= 0) end = std::min(rows.size(), begin + size_t(limit));

    // ---- project ----
    std::vector<std::vector<Value>> out;
    for (size_t i = begin; i < end; ++i) {
        RowContext ctx{table, rows[i].rowid, &rows[i].values};
        std::vector<Value> row;
        if (s.star) {
            row = rows[i].values;
        } else {
            for (const ExprPtr& e : s.columns) row.push_back(eval_expr(*e, &ctx));
        }
        out.push_back(std::move(row));
    }
    return ExecResult::table(std::move(cols), std::move(out));
}

// =====================================================================
// UPDATE / DELETE
// =====================================================================

ExecResult exec_update(Pager& pager, Catalog& catalog, const UpdateStmt& s) {
    const TableDef* table = catalog.find_table(s.table);
    if (!table) throw DbError::exec(str("no such table: ", s.table));

    // resolve SET targets
    std::vector<std::pair<int, const Expr*>> sets;
    for (const auto& [name, expr] : s.sets) {
        int idx = table->column_index(name);
        if (idx < 0)
            throw DbError::exec(str("no such column: ", name, " (table '", table->name, "')"));
        sets.emplace_back(idx, expr.get());
    }
    if (s.where) validate_columns(*s.where, *table);
    for (const auto& [name, expr] : s.sets) validate_columns(*expr, *table);

    Plan plan = plan_query(catalog, *table, s.where.get());
    std::vector<FetchedRow> rows = filter_rows(fetch_rows(pager, plan), *table,
                                               s.where.get());

    BTree<RowIdKey> ttree(pager, table->root_page);
    RowIdKey maxk;
    int64_t next_auto = ttree.max_key(maxk) ? maxk.id + 1 : 1;
    uint64_t updated = 0;

    for (const FetchedRow& row : rows) {
        RowContext ctx{table, row.rowid, &row.values};
        std::vector<Value> new_values = row.values;
        for (auto [idx, expr] : sets) {
            Value v = eval_expr(*expr, &ctx);
            new_values[size_t(idx)] =
                v.apply_affinity(table->columns[size_t(idx)].type,
                                 table->columns[size_t(idx)].name);
        }

        // new row id (INTEGER PRIMARY KEY may change)
        int64_t new_rowid = row.rowid;
        if (table->ipk_index >= 0) {
            const Value& pk = new_values[size_t(table->ipk_index)];
            if (!pk.is_null()) new_rowid = pk.as_int();
            else new_rowid = next_auto++;
        }
        bool rowid_changed = new_rowid != row.rowid;

        if (rowid_changed) {
            std::vector<uint8_t> probe;
            if (ttree.search(RowIdKey(new_rowid), probe))
                throw DbError::constraint(str(
                    "PRIMARY KEY '", table->name, ".",
                    table->ipk_index >= 0
                        ? table->columns[size_t(table->ipk_index)].name
                        : std::string("rowid"),
                    "' already contains value ", new_rowid));
        }

        // index maintenance: rebuild entries whose key changed
        for (const IndexDef* ix : catalog.indexes_on_table(table->name)) {
            int col = table->column_index(ix->column);
            if (col < 0)
                throw DbError::storage(str("index '", ix->name, "' references unknown column"));
            const Value& old_v = row.values[size_t(col)];
            const Value& new_v = new_values[size_t(col)];
            if (rowid_changed || !old_v.same_as(new_v)) {
                BTree<IndexKey> itree(pager, ix->root_page);
                if (!itree.remove(IndexKey(old_v, row.rowid)))
                    throw DbError::storage(str("index/table inconsistency: index '", ix->name,
                                               "' is missing the entry for row ", row.rowid));
                itree.insert(IndexKey(new_v, new_rowid), nullptr);
            }
        }

        std::vector<Value> rec = new_values;
        if (table->ipk_index >= 0) rec[size_t(table->ipk_index)] = Value::null();
        auto payload = encode_record(rec);

        if (rowid_changed) {
            if (!ttree.remove(RowIdKey(row.rowid)))
                throw DbError::storage(str("internal error: row ", row.rowid,
                                           " vanished during update"));
            ttree.insert(RowIdKey(new_rowid), &payload);
        } else {
            ttree.update_payload(RowIdKey(row.rowid), payload);
        }
        ++updated;
    }
    return ExecResult::ok(str(updated, " row(s) updated in '", table->name, "'"));
}

ExecResult exec_delete(Pager& pager, Catalog& catalog, const DeleteStmt& s) {
    const TableDef* table = catalog.find_table(s.table);
    if (!table) throw DbError::exec(str("no such table: ", s.table));
    if (s.where) validate_columns(*s.where, *table);

    Plan plan = plan_query(catalog, *table, s.where.get());
    std::vector<FetchedRow> rows = filter_rows(fetch_rows(pager, plan), *table,
                                               s.where.get());

    BTree<RowIdKey> ttree(pager, table->root_page);
    for (const FetchedRow& row : rows) {
        index_remove_row(pager, catalog, *table, row.values, row.rowid);
        if (!ttree.remove(RowIdKey(row.rowid)))
            throw DbError::storage(str("internal error: row ", row.rowid,
                                       " vanished during delete"));
    }
    return ExecResult::ok(str(rows.size(), " row(s) deleted from '", table->name, "'"));
}

} // namespace

ExecResult execute_statement(Pager& pager, Catalog& catalog, const Statement& stmt) {
    switch (stmt.kind) {
        case Statement::Kind::CreateTable: return exec_create_table(pager, catalog, stmt.create_table);
        case Statement::Kind::CreateIndex: return exec_create_index(pager, catalog, stmt.create_index);
        case Statement::Kind::Insert:      return exec_insert(pager, catalog, stmt.insert);
        case Statement::Kind::Select:      return exec_select(pager, catalog, stmt.select);
        case Statement::Kind::Update:      return exec_update(pager, catalog, stmt.update);
        case Statement::Kind::Delete:      return exec_delete(pager, catalog, stmt.del);
        case Statement::Kind::Explain: {
            const Statement& inner = *stmt.inner;
            switch (inner.kind) {
                case Statement::Kind::Select: {
                    if (!inner.select.has_table)
                        return ExecResult::ok("Expression evaluation (no FROM clause)");
                    const TableDef* table = catalog.find_table(inner.select.table);
                    if (!table)
                        throw DbError::exec(str("no such table: ", inner.select.table));
                    Plan plan = plan_query(catalog, *table, inner.select.where.get());
                    return ExecResult::ok(plan.describe());
                }
                case Statement::Kind::Update: {
                    const TableDef* table = catalog.find_table(inner.update.table);
                    if (!table)
                        throw DbError::exec(str("no such table: ", inner.update.table));
                    Plan plan = plan_query(catalog, *table, inner.update.where.get());
                    return ExecResult::ok(plan.describe());
                }
                case Statement::Kind::Delete: {
                    const TableDef* table = catalog.find_table(inner.del.table);
                    if (!table)
                        throw DbError::exec(str("no such table: ", inner.del.table));
                    Plan plan = plan_query(catalog, *table, inner.del.where.get());
                    return ExecResult::ok(plan.describe());
                }
                default:
                    return ExecResult::ok("DDL statement (no query plan)");
            }
        }
        case Statement::Kind::Begin:
        case Statement::Kind::Commit:
        case Statement::Kind::Rollback:
            throw DbError::txn(
                "internal error: transaction statements are handled by Database::execute");
    }
    throw DbError::exec("internal error: unknown statement kind");
}

} // namespace sc
