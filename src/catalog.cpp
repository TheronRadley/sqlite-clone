#include "catalog.hpp"

#include "util.hpp"

namespace sc {

Catalog::Catalog(Pager& pager) : pager_(pager), tree_(pager, 1) {
    reload();
}

void Catalog::reload() {
    tables_.clear();
    indexes_.clear();

    auto cur = tree_.cursor();
    typename BTree<RowIdKey>::Entry e;
    while (cur.next(e)) {
        CatalogEntry ce = decode_catalog_entry(e.key.id, e.payload);
        if (ce.kind == CatalogEntry::Kind::Table) {
            TableDef t = std::move(ce.table);
            if (t.sql.empty()) t.sql = table_def_sql(t);
            tables_[lower(t.name)] = std::move(t);
        } else {
            IndexDef i = std::move(ce.index);
            if (i.sql.empty()) i.sql = index_def_sql(i);
            indexes_[lower(i.name)] = std::move(i);
        }
    }
}

const TableDef* Catalog::find_table(const std::string& name) const {
    auto it = tables_.find(lower(name));
    return it == tables_.end() ? nullptr : &it->second;
}

TableDef* Catalog::find_table_mut(const std::string& name) {
    auto it = tables_.find(lower(name));
    return it == tables_.end() ? nullptr : &it->second;
}

const IndexDef* Catalog::find_index(const std::string& name) const {
    auto it = indexes_.find(lower(name));
    return it == indexes_.end() ? nullptr : &it->second;
}

const TableDef* Catalog::find_table_by_root(uint32_t root) const {
    for (const auto& [k, t] : tables_)
        if (t.root_page == root) return &t;
    return nullptr;
}

const IndexDef* Catalog::find_index_by_root(uint32_t root) const {
    for (const auto& [k, i] : indexes_)
        if (i.root_page == root) return &i;
    return nullptr;
}

std::vector<const TableDef*> Catalog::all_tables() const {
    std::vector<const TableDef*> out;
    for (const auto& [k, t] : tables_) out.push_back(&t);
    return out;
}

std::vector<const IndexDef*> Catalog::all_indexes() const {
    std::vector<const IndexDef*> out;
    for (const auto& [k, i] : indexes_) out.push_back(&i);
    return out;
}

std::vector<const IndexDef*> Catalog::indexes_on_table(const std::string& table) const {
    std::vector<const IndexDef*> out;
    std::string t = lower(table);
    for (const auto& [k, i] : indexes_)
        if (lower(i.table) == t) out.push_back(&i);
    return out;
}

void Catalog::add_table(TableDef def) {
    if (find_table(def.name) || find_index(def.name))
        throw DbError::constraint(str("table '", def.name, "' already exists"));
    if (def.root_page == 0)
        throw DbError::exec("internal error: table without a root page");

    // rowid = current max + 1
    RowIdKey max;
    int64_t next = 1;
    if (tree_.max_key(max)) next = max.id + 1;
    if (next > INT32_MAX) throw DbError::exec("catalog rowid space exhausted");

    auto payload = encode_table_entry(def);
    auto res = tree_.insert(RowIdKey(next), &payload);
    if (res != BTree<RowIdKey>::InsertResult::Inserted)
        throw DbError::storage("internal error: duplicate catalog rowid");
    tables_[lower(def.name)] = std::move(def);
}

void Catalog::add_index(IndexDef def) {
    if (find_index(def.name) || find_table(def.name))
        throw DbError::constraint(str("index '", def.name, "' already exists"));
    if (def.root_page == 0)
        throw DbError::exec("internal error: index without a root page");

    RowIdKey max;
    int64_t next = 1;
    if (tree_.max_key(max)) next = max.id + 1;
    if (next > INT32_MAX) throw DbError::exec("catalog rowid space exhausted");

    auto payload = encode_index_entry(def);
    auto res = tree_.insert(RowIdKey(next), &payload);
    if (res != BTree<RowIdKey>::InsertResult::Inserted)
        throw DbError::storage("internal error: duplicate catalog rowid");
    indexes_[lower(def.name)] = std::move(def);
}

} // namespace sc
