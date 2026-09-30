#include "record.hpp"
#include "util.hpp"

#include "varint.hpp"

namespace sc {

namespace {

// Smallest serial type (1..6) that can hold `v`.
int int_serial_type(int64_t v) {
    if (v >= -128 && v <= 127) return 1;
    if (v >= -32768 && v <= 32767) return 2;
    if (v >= -8388608 && v <= 8388607) return 3;
    if (v >= -2147483648ll && v <= 2147483647ll) return 4;
    if (v >= -140737488355328ll && v <= 140737488355327ll) return 5;
    return 6;
}

inline int int_width(int serial) {
    static const int w[7] = {0, 1, 2, 3, 4, 6, 8};
    return w[serial];
}

// Two's-complement big-endian of the minimal width.
void put_int_body(std::vector<uint8_t>& out, int64_t v, int width) {
    uint8_t buf[8];
    wr_be64(buf, uint64_t(v));
    out.insert(out.end(), buf + (8 - width), buf + 8);
}

int64_t get_int_body(const uint8_t* p, int width) {
    uint8_t buf[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    // sign-extend
    uint8_t fill = (p[0] & 0x80) ? 0xff : 0x00;
    for (int i = 0; i < 8 - width; ++i) buf[i] = fill;
    for (int i = 0; i < width; ++i) buf[8 - width + i] = p[i];
    return int64_t(rd_be64(buf));
}

} // namespace

std::vector<uint8_t> encode_record(const std::vector<Value>& values) {
    // serial types first (to size the header)
    std::vector<uint64_t> serials;
    serials.reserve(values.size());
    for (const Value& v : values) {
        if (v.is_null()) serials.push_back(0);
        else if (v.is_int()) serials.push_back(uint64_t(int_serial_type(v.as_int())));
        else serials.push_back(13 + 2 * v.as_text().size());
    }

    // header size includes the size varint itself; solve the fixed point.
    size_t header_size = 1;
    for (;;) {
        size_t n = varint_size(header_size);
        for (uint64_t s : serials) n += varint_size(s);
        if (n == header_size) break;
        header_size = n;
        if (header_size > 1 << 20)
            throw DbError::storage("record header size overflow");
    }

    std::vector<uint8_t> out;
    out.reserve(header_size + 16);
    put_varint(out, header_size);
    for (uint64_t s : serials) put_varint(out, s);
    if (out.size() != header_size)
        throw DbError::storage("internal: record header size mismatch");
    for (const Value& v : values) {
        if (v.is_null()) {
            // nothing
        } else if (v.is_int()) {
            put_int_body(out, v.as_int(), int_width(int_serial_type(v.as_int())));
        } else {
            const std::string& s = v.as_text();
            out.insert(out.end(), s.begin(), s.end());
        }
    }
    return out;
}

Record decode_record(const uint8_t* data, size_t len) {
    if (len == 0) throw DbError::storage("malformed record: zero length");
    size_t off = 0;
    size_t used = 0;
    uint64_t header_size = get_varint(data, len, used);
    off += used;
    if (header_size < 1 || header_size > len)
        throw DbError::storage(str("malformed record: header size ", header_size,
                                   " exceeds record length ", len));

    std::vector<uint64_t> serials;
    while (off < header_size) {
        if (off >= len) throw DbError::storage("malformed record: truncated header");
        uint64_t s = get_varint(data + off, len - off, used);
        off += used;
        serials.push_back(s);
    }

    Record rec;
    rec.values.reserve(serials.size());
    for (uint64_t s : serials) {
        if (s == 0) {
            rec.values.push_back(Value::null());
        } else if (s >= 1 && s <= 6) {
            int w = int_width(int(s));
            if (off + w > len)
                throw DbError::storage("malformed record: truncated integer value");
            rec.values.push_back(Value::integer(get_int_body(data + off, w)));
            off += w;
        } else if (s >= 13 && (s & 1) == 1) {
            size_t n = size_t((s - 13) / 2);
            if (off + n > len)
                throw DbError::storage("malformed record: truncated text value");
            rec.values.emplace_back(
                Value::text(std::string(reinterpret_cast<const char*>(data + off), n)));
            off += n;
        } else {
            throw DbError::storage(str("malformed record: unknown serial type ", s));
        }
    }
    if (off != len)
        throw DbError::storage(str("malformed record: ", len - off,
                                   " trailing bytes after last value"));
    return rec;
}

} // namespace sc
