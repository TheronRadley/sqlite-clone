#include "schema.hpp"

#include "lexer.hpp"
#include "util.hpp"

namespace sc {

int TableDef::column_index(const std::string& col) const {
    std::string want = lower(col);
    for (size_t i = 0; i < columns.size(); ++i)
        if (lower(columns[i].name) == want) return int(i);
    return -1;
}

std::vector<uint8_t> encode_table_entry(const TableDef& t) {
    std::vector<Value> rec;
    rec.push_back(Value::integer(1));
    rec.push_back(Value::text(t.name));
    rec.push_back(Value::integer(int64_t(t.root_page)));
    rec.push_back(Value::text(t.sql));
    rec.push_back(Value::integer(int64_t(t.columns.size())));
    for (const ColumnDef& c : t.columns) {
        rec.push_back(Value::text(c.name));
        rec.push_back(Value::integer(c.type == ColType::Integer ? 1 : 2));
        rec.push_back(Value::integer(c.primary_key ? 1 : 0));
    }
    return encode_record(rec);
}

std::vector<uint8_t> encode_index_entry(const IndexDef& i) {
    std::vector<Value> rec;
    rec.push_back(Value::integer(2));
    rec.push_back(Value::text(i.name));
    rec.push_back(Value::integer(int64_t(i.root_page)));
    rec.push_back(Value::text(i.sql));
    rec.push_back(Value::text(i.table));
    rec.push_back(Value::text(i.column));
    return encode_record(rec);
}

CatalogEntry decode_catalog_entry(int64_t rowid, const std::vector<uint8_t>& payload) {
    Record r = decode_record(payload);
    const auto& v = r.values;
    auto bad = [&](const std::string& what) {
        return DbError::storage(str("corrupt catalog entry (rowid ", rowid, "): ", what));
    };
    if (v.size() < 5) throw bad("too few fields");
    if (!v[0].is_int()) throw bad("kind field is not an integer");
    CatalogEntry e;
    if (v[0].as_int() == 1) {
        e.kind = CatalogEntry::Kind::Table;
        TableDef& t = e.table;
        if (!v[1].is_text() || !v[2].is_int() || !v[3].is_text() || !v[4].is_int())
            throw bad("malformed table fields");
        t.name = v[1].as_text();
        t.root_page = uint32_t(v[2].as_int());
        t.sql = v[3].as_text();
        int64_t ncols = v[4].as_int();
        if (ncols < 0 || v.size() != size_t(5 + 3 * ncols))
            throw bad("column count does not match record size");
        for (int64_t i = 0; i < ncols; ++i) {
            const Value& cn = v[size_t(5 + 3 * i)];
            const Value& ct = v[size_t(6 + 3 * i)];
            const Value& cp = v[size_t(7 + 3 * i)];
            if (!cn.is_text() || !ct.is_int() || !cp.is_int())
                throw bad("malformed column tuple");
            ColumnDef c;
            c.name = cn.as_text();
            if (c.name.empty()) throw bad("empty column name");
            if (ct.as_int() == 1) c.type = ColType::Integer;
            else if (ct.as_int() == 2) c.type = ColType::Text;
            else throw bad("unknown column type code");
            c.primary_key = cp.as_int() != 0;
            if (c.primary_key) {
                if (t.ipk_index >= 0) throw bad("multiple primary key columns");
                if (c.type != ColType::Integer)
                    throw bad("primary key on a non-INTEGER column");
                t.ipk_index = int(i);
            }
            t.columns.push_back(std::move(c));
        }
        if (t.root_page < 2 || t.root_page >= (1 << 30)) throw bad("bad root page");
    } else if (v[0].as_int() == 2) {
        e.kind = CatalogEntry::Kind::Index;
        IndexDef& ix = e.index;
        if (v.size() != 6 || !v[1].is_text() || !v[2].is_int() || !v[3].is_text() ||
            !v[4].is_text() || !v[5].is_text())
            throw bad("malformed index fields");
        ix.name = v[1].as_text();
        ix.root_page = uint32_t(v[2].as_int());
        ix.sql = v[3].as_text();
        ix.table = v[4].as_text();
        ix.column = v[5].as_text();
        if (ix.root_page < 2) throw bad("bad root page");
    } else {
        throw bad("unknown entry kind");
    }
    return e;
}

std::string table_def_sql(const TableDef& t) {
    std::string out = "CREATE TABLE " + t.name + " (";
    for (size_t i = 0; i < t.columns.size(); ++i) {
        if (i) out += ", ";
        const ColumnDef& c = t.columns[i];
        out += c.name + " " + col_type_name(c.type);
        if (c.primary_key) out += " PRIMARY KEY";
    }
    out += ");";
    return out;
}

std::string index_def_sql(const IndexDef& i) {
    return "CREATE INDEX " + i.name + " ON " + i.table + "(" + i.column + ");";
}

} // namespace sc
