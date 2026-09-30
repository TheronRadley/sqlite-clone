// record.hpp — the row/record serialization format.
//
// A record is a sequence of Values (NULL / INTEGER / TEXT) with a
// SQLite-style self-describing header (documented in STORAGE_FORMAT.md):
//
//   header:  varint(header_size)  serial_type varint per column
//   body:    value bytes, concatenated
//
// serial types:
//   0        NULL            (0 bytes)
//   1..6     INTEGER         (1, 2, 3, 4, 6, or 8 bytes, big-endian two's complement)
//   13+2n    TEXT            (n bytes, UTF-8)
//
// The minimal integer width is chosen on encode; decoding sign-extends.
// Every read is bounds-checked: a malformed record raises StorageError.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "value.hpp"

namespace sc {

std::vector<uint8_t> encode_record(const std::vector<Value>& values);

struct Record {
    std::vector<Value> values;
};

// Decode exactly `len` bytes. Throws StorageError if the bytes are not a
// well-formed record (bad header size, bad serial type, truncated body).
Record decode_record(const uint8_t* data, size_t len);

inline Record decode_record(const std::vector<uint8_t>& v) {
    return decode_record(v.data(), v.size());
}

} // namespace sc
