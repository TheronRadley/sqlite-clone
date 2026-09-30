// schema.hpp — table/index definitions and their on-disk catalog encoding.
//
// The catalog itself is a table B-Tree rooted at page 1 (see
// STORAGE_FORMAT.md). Each row is a record:
//
//   table entry: [1, name, root_page, sql, ncols,
//                 (col_name, col_type, is_pk) * ncols]
//   index entry: [2, name, root_page, sql, table_name, column_name]
//
// INTEGER PRIMARY KEY columns are rowid aliases (SQLite semantics): the
// record stores NULL in that column and the real value is the B-Tree key.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "record.hpp"
#include "value.hpp"

namespace sc {

struct ColumnDef {
    std::string name;
    ColType type;
    bool primary_key = false;
};

struct TableDef {
    std::string name;             // original spelling
    uint32_t root_page = 0;
    std::vector<ColumnDef> columns;
    int ipk_index = -1;           // index of the INTEGER PRIMARY KEY column
    std::string sql;              // canonical CREATE TABLE text

    // case-insensitive column lookup; -1 when absent
    int column_index(const std::string& col) const;
    bool has_rowid_alias() const { return ipk_index >= 0; }
};

struct IndexDef {
    std::string name;
    std::string table;
    std::string column;
    uint32_t root_page = 0;
    std::string sql;
};

// ---- catalog record codec ----
std::vector<uint8_t> encode_table_entry(const TableDef& t);
std::vector<uint8_t> encode_index_entry(const IndexDef& i);

struct CatalogEntry {
    enum class Kind { Table, Index } kind;
    TableDef table;
    IndexDef index;
};
CatalogEntry decode_catalog_entry(int64_t rowid, const std::vector<uint8_t>& payload);

// canonical SQL text (used by .schema and .dump)
std::string table_def_sql(const TableDef& t);
std::string index_def_sql(const IndexDef& i);

} // namespace sc
