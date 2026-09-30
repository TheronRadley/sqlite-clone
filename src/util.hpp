// util.hpp — small shared helpers: big-endian byte access, checksums,
// checked arithmetic, string building.
//
// All multi-byte on-disk integers are BIG-ENDIAN. There is exactly one
// place in the codebase that converts between bytes and integers: here.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>
#include <sstream>
#include <vector>

namespace sc {

// ---------- big-endian readers (bounds must be pre-validated by callers) ----------

inline uint16_t rd_be16(const uint8_t* p) {
    return static_cast<uint16_t>((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}

inline uint32_t rd_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline uint64_t rd_be64(const uint8_t* p) {
    return (uint64_t(rd_be32(p)) << 32) | rd_be32(p + 4);
}

// ---------- big-endian writers ----------

inline void wr_be16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}

inline void wr_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

inline void wr_be64(uint8_t* p, uint64_t v) {
    wr_be32(p, uint32_t(v >> 32));
    wr_be32(p + 4, uint32_t(v));
}

// ---------- FNV-1a 64-bit checksum (used for file-header and journal integrity) ----------

inline uint64_t fnv1a64(const uint8_t* p, size_t n) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// ---------- checked integer arithmetic (never rely on wrap-around for user data) ----------

inline bool add_ovf(int64_t a, int64_t b, int64_t* r) { return __builtin_add_overflow(a, b, r); }
inline bool sub_ovf(int64_t a, int64_t b, int64_t* r) { return __builtin_sub_overflow(a, b, r); }
inline bool mul_ovf(int64_t a, int64_t b, int64_t* r) { return __builtin_mul_overflow(a, b, r); }

// ---------- string building ----------

template <class... A>
std::string str(A&&... a) {
    std::ostringstream o;
    (o << ... << std::forward<A>(a));
    return o.str();
}

inline std::string to_hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 0xf]);
    }
    return s;
}

inline std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string quote_sql_string(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        out.push_back(c);
        if (c == '\'') out.push_back('\'');
    }
    out.push_back('\'');
    return out;
}

} // namespace sc
