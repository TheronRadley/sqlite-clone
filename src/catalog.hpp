// catalog.hpp — the persistent schema manager.
//
// Owns the in-memory schema mirror (name -> definition) and the catalog
// B-Tree at page 1. All mutations go through add_table/add_index, which
// write catalog rows through the pager (so they participate in
// transactions like any other page).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "btree.hpp"
#include "pager.hpp"
#include "schema.hpp"

namespace sc {

class Catalog {
public:
    // Loads (or initializes) the catalog tree rooted at page 1.
    explicit Catalog(Pager& pager);

    void reload();

    // case-insensitive lookups; nullptr when absent
    const TableDef* find_table(const std::string& name) const;
    TableDef* find_table_mut(const std::string& name);
    const IndexDef* find_index(const std::string& name) const;
    const TableDef* find_table_by_root(uint32_t root) const;
    const IndexDef* find_index_by_root(uint32_t root) const;

    std::vector<const TableDef*> all_tables() const;
    std::vector<const IndexDef*> all_indexes() const;
    std::vector<const IndexDef*> indexes_on_table(const std::string& table) const;

    // Insert a new catalog entry (call inside a transaction). Takes
    // ownership of root pages: the tree row is appended to the catalog
    // B-Tree and the in-memory mirror is updated.
    void add_table(TableDef def);
    void add_index(IndexDef def);

    BTree<RowIdKey>& tree() { return tree_; }
    const BTree<RowIdKey>& tree() const { return tree_; }

private:
    Pager& pager_;
    BTree<RowIdKey> tree_;
    std::map<std::string, TableDef> tables_;    // key: lowercased name
    std::map<std::string, IndexDef> indexes_;
};

} // namespace sc
