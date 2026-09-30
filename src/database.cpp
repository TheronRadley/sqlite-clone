// database.cpp — engine facade: open/close, transaction protocol,
// introspection, whole-database validation.
#include "database.hpp"

#include <set>
#include <sstream>

#include "expr.hpp"
#include "parser.hpp"
#include "planner.hpp"
#include "util.hpp"

namespace sc {

Database::Database(std::unique_ptr<Pager> pager) : pager_(std::move(pager)) {
    catalog_ = std::make_unique<Catalog>(*pager_);
}

std::unique_ptr<Database> Database::open(const std::string& path, uint32_t page_size) {
    return std::unique_ptr<Database>(
        new Database(Pager::open_file(path, page_size)));
}

std::unique_ptr<Database> Database::open_memory(uint32_t page_size) {
    return std::unique_ptr<Database>(
        new Database(Pager::open_memory(page_size)));
}

Database::~Database() {
    if (pager_ && pager_->in_txn()) {
        try {
            pager_->rollback_txn();
        } catch (...) {
            // best effort: dropping the database rolls back like a crash
        }
    }
}

std::vector<ExecResult> Database::execute(const std::string& sql) {
    auto stmts = parse(sql);
    std::vector<ExecResult> out;
    out.reserve(stmts.size());
    for (const auto& stmt : stmts) {
        switch (stmt->kind) {
            case Statement::Kind::Begin:
                if (pager_->in_txn())
                    throw DbError::txn("cannot start a transaction within a transaction");
                pager_->begin_txn();
                out.push_back(ExecResult::ok("transaction started"));
                break;
            case Statement::Kind::Commit:
                pager_->commit_txn();
                out.push_back(ExecResult::ok("transaction committed"));
                break;
            case Statement::Kind::Rollback:
                pager_->rollback_txn();
                out.push_back(ExecResult::ok("transaction rolled back"));
                break;
            default: {
                bool autocommit = !pager_->in_txn();
                if (autocommit) pager_->begin_txn();
                else pager_->begin_stmt();
                try {
                    ExecResult r = execute_statement(*pager_, *catalog_, *stmt);
                    if (autocommit) pager_->commit_txn();
                    else pager_->end_stmt();
                    out.push_back(std::move(r));
                } catch (...) {
                    // undo this statement only; the error propagates
                    if (autocommit) {
                        try {
                            pager_->rollback_txn();
                        } catch (...) {
                        }
                    } else {
                        pager_->rollback_stmt();
                    }
                    throw;
                }
                break;
            }
        }
    }
    return out;
}

// =====================================================================
// Introspection
// =====================================================================

std::vector<std::string> Database::table_names() const {
    std::vector<std::string> out;
    for (const TableDef* t : catalog_->all_tables()) out.push_back(t->name);
    return out;
}

std::vector<std::string> Database::index_names() const {
    std::vector<std::string> out;
    for (const IndexDef* i : catalog_->all_indexes()) out.push_back(i->name);
    return out;
}

std::string Database::schema_sql() const {
    std::ostringstream out;
    for (const TableDef* t : catalog_->all_tables()) out << t->sql << "\n";
    for (const IndexDef* i : catalog_->all_indexes()) out << i->sql << "\n";
    return out.str();
}

std::string Database::dump_sql() {
    std::ostringstream out;
    out << "-- sqlite-clone database dump\n";
    out << "BEGIN;\n";
    for (const TableDef* t : catalog_->all_tables()) {
        out << t->sql << "\n";
        BTree<RowIdKey> ttree(*pager_, t->root_page);
        auto cur = ttree.cursor();
        BTree<RowIdKey>::Entry e;
        while (cur.next(e)) {
            Record rec = decode_record(e.payload);
            if (rec.values.size() != t->columns.size())
                throw DbError::storage(str("table '", t->name, "': row ", e.key.id,
                                           " does not match the schema"));
            if (t->ipk_index >= 0)
                rec.values[size_t(t->ipk_index)] = Value::integer(e.key.id);
            out << "INSERT INTO " << t->name << " VALUES (";
            for (size_t i = 0; i < rec.values.size(); ++i) {
                if (i) out << ", ";
                out << rec.values[i].sql_literal();
            }
            out << ");\n";
        }
    }
    for (const IndexDef* i : catalog_->all_indexes()) out << i->sql << "\n";
    out << "COMMIT;\n";
    return out.str();
}

std::vector<Database::PageInfo> Database::page_inventory() {
    std::map<uint32_t, std::string> kinds;
    kinds[0] = "header";
    kinds[1] = "catalog root";

    auto claim = [&](uint32_t id, const std::string& kind) {
        auto it = kinds.find(id);
        if (it != kinds.end() && it->second != "header" && it->second != "catalog root")
            throw DbError::storage(str("page ", id, " is claimed by both '", it->second,
                                       "' and '", kind, "'"));
        kinds[id] = kind;
    };

    for (const TableDef* t : catalog_->all_tables()) {
        auto rep = BTree<RowIdKey>(*pager_, t->root_page).validate();
        for (uint32_t id : rep.page_ids) claim(id, "table:" + t->name);
    }
    for (const IndexDef* i : catalog_->all_indexes()) {
        auto rep = BTree<IndexKey>(*pager_, i->root_page).validate();
        for (uint32_t id : rep.page_ids) claim(id, "index:" + i->name);
    }
    {
        auto rep = catalog_->tree().validate();
        for (uint32_t id : rep.page_ids) claim(id, "catalog");
    }
    for (uint32_t id : freelist::collect(*pager_)) claim(id, "free");

    std::vector<PageInfo> out;
    for (uint32_t id = 0; id < pager_->page_count(); ++id) {
        auto it = kinds.find(id);
        if (it != kinds.end()) {
            out.push_back(PageInfo{id, it->second});
        } else {
            out.push_back(PageInfo{id, "UNREFERENCED"});
        }
    }
    return out;
}

std::string Database::render_btree(const std::string& name) {
    if (const TableDef* t = catalog_->find_table(name))
        return BTree<RowIdKey>(*pager_, t->root_page).render();
    if (const IndexDef* i = catalog_->find_index(name))
        return BTree<IndexKey>(*pager_, i->root_page).render();
    throw DbError::exec(str("no such table or index: ", name));
}

// =====================================================================
// Validation
// =====================================================================

void Database::validate(bool deep) {
    // 1. freelist structure and counts
    freelist::validate(*pager_);

    // 2. every tree validates independently
    auto cat_rep = catalog_->tree().validate();

    std::set<uint32_t> used{0};
    for (uint32_t id : cat_rep.page_ids) {
        if (id == 0)
            throw DbError::storage("the catalog tree claims the header page");
        if (!used.insert(id).second)
            throw DbError::storage(str("page ", id, " is shared between the catalog and "
                                                    "another structure"));
    }

    for (const TableDef* t : catalog_->all_tables()) {
        auto rep = BTree<RowIdKey>(*pager_, t->root_page).validate();
        for (uint32_t id : rep.page_ids) {
            if (id < 2)
                throw DbError::storage(str("tree of table '", t->name,
                                           "' claims reserved page ", id));
            if (!used.insert(id).second)
                throw DbError::storage(str("page ", id, " is referenced by the tree of '",
                                           t->name, "' and another structure"));
        }
    }
    for (const IndexDef* i : catalog_->all_indexes()) {
        auto rep = BTree<IndexKey>(*pager_, i->root_page).validate();
        for (uint32_t id : rep.page_ids) {
            if (id < 2)
                throw DbError::storage(str("tree of index '", i->name,
                                           "' claims reserved page ", id));
            if (!used.insert(id).second)
                throw DbError::storage(str("page ", id, " is referenced by the tree of '",
                                           i->name, "' and another structure"));
        }
    }

    // 3. freelist pages are disjoint from tree pages
    for (uint32_t id : freelist::collect(*pager_)) {
        if (!used.insert(id).second)
            throw DbError::storage(str("page ", id,
                                       " is both in the freelist and in use by a tree"));
    }

    // 4. no orphans: every page of the file is accounted for
    for (uint32_t id = 0; id < pager_->page_count(); ++id)
        if (!used.count(id))
            throw DbError::storage(str("page ", id,
                                       " is not referenced by any tree or the freelist "
                                       "(orphaned/leaked page)"));

    if (!deep) return;

    // 5. index/table consistency, both directions
    for (const TableDef* t : catalog_->all_tables()) {
        BTree<RowIdKey> ttree(*pager_, t->root_page);
        auto indexes = catalog_->indexes_on_table(t->name);
        auto cur = ttree.cursor();
        BTree<RowIdKey>::Entry e;
        while (cur.next(e)) {
            Record rec = decode_record(e.payload);
            if (rec.values.size() != t->columns.size())
                throw DbError::storage(str("table '", t->name, "': row ", e.key.id,
                                           " does not match the schema"));
            if (t->ipk_index >= 0)
                rec.values[size_t(t->ipk_index)] = Value::integer(e.key.id);
            for (const IndexDef* ix : indexes) {
                int col = t->column_index(ix->column);
                if (col < 0)
                    throw DbError::storage(str("index '", ix->name,
                                               "' references unknown column"));
                BTree<IndexKey> itree(*pager_, ix->root_page);
                std::vector<uint8_t> unused;
                if (!itree.search(IndexKey(rec.values[size_t(col)], e.key.id), unused))
                    throw DbError::storage(str("index '", ix->name, "' is missing (",
                                               rec.values[size_t(col)].sql_literal(), ", ",
                                               e.key.id, ") for table '", t->name, "'"));
            }
        }
        for (const IndexDef* ix : indexes) {
            BTree<IndexKey> itree(*pager_, ix->root_page);
            int col = t->column_index(ix->column);
            auto icur = itree.cursor();
            BTree<IndexKey>::Entry ie;
            while (icur.next(ie)) {
                std::vector<uint8_t> payload;
                if (!ttree.search(RowIdKey(ie.key.rowid), payload))
                    throw DbError::storage(str("index '", ix->name, "' points at row ",
                                               ie.key.rowid, " which does not exist in '",
                                               t->name, "'"));
                Record rec = decode_record(payload);
                if (rec.values.size() != t->columns.size())
                    throw DbError::storage(str("table '", t->name, "': row ", ie.key.rowid,
                                               " does not match the schema"));
                if (t->ipk_index >= 0)
                    rec.values[size_t(t->ipk_index)] = Value::integer(ie.key.rowid);
                if (!rec.values[size_t(col)].same_as(ie.key.value))
                    throw DbError::storage(str("index '", ix->name, "' entry (",
                                               ie.key.value.sql_literal(), ", ", ie.key.rowid,
                                               ") disagrees with the table row value ",
                                               rec.values[size_t(col)].sql_literal()));
            }
        }
    }
}

} // namespace sc
