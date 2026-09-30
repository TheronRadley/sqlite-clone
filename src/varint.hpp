// varint.hpp — SQLite-style variable-length integers.
//
// Format (documented in STORAGE_FORMAT.md):
//   * 1..8 bytes: 7 bits per byte, most-significant group first.
//     The high bit of every byte except the LAST is set (continuation).
//   * 9-byte form: eight 7-bit groups (high bits set) followed by one raw
//     byte of 8 bits. Used for values >= 2^56.
//
// Varints are used for record headers and payload lengths. They are
// self-delimiting: a reader knows it is done when it sees a byte without
// the continuation bit (or after the 9th byte).
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

#include "error.hpp"

namespace sc {

inline constexpr size_t VARINT_MAX = 9;

// Number of bytes needed to encode `value`.
inline size_t varint_size(uint64_t value) {
    if (value > 0x00ff'ffff'ffff'ffffull) return 9;
    size_t n = 1;
    while ((value >>= 7) != 0) ++n;
    return n;
}

// Encode; appends to `out`; returns bytes written.
inline size_t put_varint(std::vector<uint8_t>& out, uint64_t value) {
    if (value > 0x00ff'ffff'ffff'ffffull) {
        uint8_t tmp[9];
        uint64_t v = value;
        tmp[8] = uint8_t(v & 0xff);
        v >>= 8;
        for (int i = 7; i >= 0; --i) {
            tmp[i] = uint8_t(0x80 | (v & 0x7f));
            v >>= 7;
        }
        out.insert(out.end(), tmp, tmp + 9);
        return 9;
    }
    uint8_t tmp[8];
    size_t n = 0;
    uint64_t v = value;
    for (;;) {
        tmp[n++] = uint8_t(v & 0x7f);
        v >>= 7;
        if (v == 0) break;
    }
    for (size_t i = n; i-- > 0;)
        out.push_back(i == 0 ? tmp[i] : uint8_t(tmp[i] | 0x80));
    return n;
}

// Write varint into a fixed buffer (no vector).
inline size_t put_varint_buf(uint8_t* buf, uint64_t value) {
    if (value > 0x00ff'ffff'ffff'ffffull) {
        uint64_t v = value;
        buf[8] = uint8_t(v & 0xff);
        v >>= 8;
        for (int i = 7; i >= 0; --i) {
            buf[i] = uint8_t(0x80 | (v & 0x7f));
            v >>= 7;
        }
        return 9;
    }
    uint8_t tmp[8];
    size_t n = 0;
    uint64_t v = value;
    for (;;) {
        tmp[n++] = uint8_t(v & 0x7f);
        v >>= 7;
        if (v == 0) break;
    }
    for (size_t i = 0; i < n; ++i) buf[i] = (i == n - 1) ? tmp[i] : uint8_t(tmp[i] | 0x80);
    return n;
}

// Decode at most 9 bytes starting at data[0]. `avail` must be >= 1.
// Returns the value; `used` receives the number of bytes consumed.
// Throws StorageError on truncation (avail < needed).
inline uint64_t get_varint(const uint8_t* data, size_t avail, size_t& used) {
    uint64_t result = 0;
    for (size_t i = 0; i < VARINT_MAX; ++i) {
        if (i >= avail)
            throw DbError::storage("malformed varint: truncated");
        uint8_t b = data[i];
        if (i == 8) {
            result = (result << 8) | b;
            used = 9;
            return result;
        }
        result = (result << 7) | uint64_t(b & 0x7f);
        if ((b & 0x80) == 0) {
            used = i + 1;
            return result;
        }
    }
    throw DbError::storage("malformed varint: too many continuation bytes");
}

// Convenience: decode directly from a vector with an advancing offset.
inline uint64_t get_varint_vec(const std::vector<uint8_t>& v, size_t& off) {
    size_t used = 0;
    uint64_t r = get_varint(v.data() + off, v.size() - off, used);
    off += used;
    return r;
}

} // namespace sc
