// keys.hpp — order-preserving B-Tree key encodings.
//
// Two key kinds share the B-Tree implementation (see btree.hpp):
//
//   RowIdKey — table trees. Key = the 64-bit row id. Encoded as 8 bytes:
//              byte-swapped sign-flipped big-endian so that UNSIGNED
//              lexicographic byte order == signed numeric order
//              (x ^ 0x8000.. flips the sign bit).
//
//   IndexKey — index trees. Key = (indexed value, rowid). Encoded as
//              tag + value + rowid-key bytes:
//                tag 0: NULL
//                tag 1: INTEGER  (8 bytes, sign-flipped BE)
//                tag 2: TEXT     (varint length + bytes)
//              The encoding is self-delimiting. Ordering is STRUCTURAL
//              (value first with NULL < INTEGER < TEXT, then rowid), not
//              byte-wise, because no byte encoding of text preserves
//              lexicographic order across lengths (see B_TREE.md).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "util.hpp"
#include "value.hpp"
#include "varint.hpp"

namespace sc {

enum class NodeKind { Table, Index };

struct RowIdKey {
    static constexpr NodeKind node_kind = NodeKind::Table;
    int64_t id = 0;

    RowIdKey() = default;
    explicit RowIdKey(int64_t i) : id(i) {}

    void encode(std::vector<uint8_t>& out) const {
        uint8_t buf[8];
        wr_be64(buf, uint64_t(id) ^ 0x8000000000000000ull);
        out.insert(out.end(), buf, buf + 8);
    }

    static constexpr size_t encoded_size() { return 8; }

    static RowIdKey decode(const uint8_t* p, size_t avail) {
        if (avail < 8)
            throw DbError::btree("malformed rowid key: need 8 bytes");
        return RowIdKey(int64_t(rd_be64(p) ^ 0x8000000000000000ull));
    }

    int compare(const RowIdKey& o) const {
        if (id < o.id) return -1;
        if (id > o.id) return 1;
        return 0;
    }

    std::string display() const { return std::to_string(id); }
};

struct IndexKey {
    static constexpr NodeKind node_kind = NodeKind::Index;
    Value value;
    int64_t rowid = 0;

    IndexKey() = default;
    IndexKey(Value v, int64_t r) : value(std::move(v)), rowid(r) {}

    void encode(std::vector<uint8_t>& out) const {
        if (value.is_null()) {
            out.push_back(0);
        } else if (value.is_int()) {
            out.push_back(1);
            uint8_t buf[8];
            wr_be64(buf, uint64_t(value.as_int()) ^ 0x8000000000000000ull);
            out.insert(out.end(), buf, buf + 8);
        } else {
            out.push_back(2);
            put_varint(out, value.as_text().size());
            const std::string& s = value.as_text();
            out.insert(out.end(), s.begin(), s.end());
        }
        uint8_t buf[8];
        wr_be64(buf, uint64_t(rowid) ^ 0x8000000000000000ull);
        out.insert(out.end(), buf, buf + 8);
    }

    // Decode from `p` (up to `avail` bytes). Returns the key; `used`
    // receives the number of bytes consumed. Self-delimiting.
    static IndexKey decode(const uint8_t* p, size_t avail, size_t& used) {
        if (avail < 1) throw DbError::btree("malformed index key: empty");
        IndexKey k;
        switch (p[0]) {
            case 0:
                k.value = Value::null();
                used = 1;
                break;
            case 1: {
                if (avail < 9) throw DbError::btree("malformed index key: short integer");
                k.value = Value::integer(int64_t(rd_be64(p + 1) ^ 0x8000000000000000ull));
                used = 9;
                break;
            }
            case 2: {
                size_t off = 1, u = 0;
                uint64_t n = get_varint(p + off, avail - off, u);
                off += u;
                if (n > avail - off)
                    throw DbError::btree("malformed index key: text longer than cell");
                k.value = Value::text(
                    std::string(reinterpret_cast<const char*>(p + off), size_t(n)));
                off += size_t(n);
                used = off;
                break;
            }
            default:
                throw DbError::btree(str("malformed index key: unknown tag ", int(p[0])));
        }
        if (avail < used + 8)
            throw DbError::btree("malformed index key: missing rowid");
        k.rowid = int64_t(rd_be64(p + used) ^ 0x8000000000000000ull);
        used += 8;
        return k;
    }

    int compare(const IndexKey& o) const {
        int c = value.compare(o.value);
        if (c != 0) return c;
        if (rowid < o.rowid) return -1;
        if (rowid > o.rowid) return 1;
        return 0;
    }

    std::string display() const {
        return "(" + value.sql_literal() + ", " + std::to_string(rowid) + ")";
    }
};

} // namespace sc
