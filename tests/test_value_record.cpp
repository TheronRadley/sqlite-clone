// test_value_record.cpp — Value semantics, varints, records, key encodings.
#include "test_util.hpp"

#include "keys.hpp"
#include <cstring>

#include "record.hpp"
#include "value.hpp"
#include "varint.hpp"

using namespace sc;

// ---------------- Value ----------------

TEST(value_ordering_null_int_text) {
    Value n = Value::null(), i = Value::integer(5), i2 = Value::integer(-7),
          t = Value::text("a");
    CHECK(n.compare(i) < 0);
    CHECK(n.compare(t) < 0);
    CHECK(i.compare(t) < 0);
    CHECK(i.compare(i2) > 0);
    CHECK(t.compare(Value::text("b")) < 0);
    CHECK(Value::text("ab").compare(Value::text("abc")) < 0);   // prefix sorts first
    CHECK(Value::text("ab").compare(Value::text("ab")) == 0);
    CHECK(n.compare(n) == 0);
}

TEST(value_affinity) {
    // TEXT -> INTEGER when well-formed
    CHECK(Value::text("30").apply_affinity(ColType::Integer, "age").as_int() == 30);
    CHECK(Value::text("-12").apply_affinity(ColType::Integer, "age").as_int() == -12);
    CHECK(Value::text("+7").apply_affinity(ColType::Integer, "age").as_int() == 7);
    CHECK(Value::integer(3).apply_affinity(ColType::Integer, "age").as_int() == 3);
    // INTEGER -> TEXT
    CHECK_EQ(Value::integer(42).apply_affinity(ColType::Text, "name").as_text(),
             std::string("42"));
    CHECK(Value::text("x").apply_affinity(ColType::Text, "name").as_text() == "x");
    // NULL stays NULL
    CHECK(Value::null().apply_affinity(ColType::Integer, "age").is_null());
    // rejects non-numeric text in INTEGER columns
    CHECK_DB_ERROR(Value::text("abc").apply_affinity(ColType::Integer, "age"),
                   int(Err::Type), "INTEGER column 'age'");
    CHECK_DB_ERROR(Value::text("").apply_affinity(ColType::Integer, "age"), int(Err::Type),
                   "INTEGER column 'age'");
    CHECK_DB_ERROR(Value::text("9223372036854775808").apply_affinity(ColType::Integer, "id"),
                   int(Err::Type), "INTEGER column 'id'");
}

// ---------------- varint ----------------

TEST(varint_roundtrip_boundaries) {
    uint64_t cases[] = {0, 1, 127, 128, 300, 16383, 16384, 2097151, 2097152,
                        (1ull << 42), (1ull << 55), (1ull << 56) - 1, (1ull << 56),
                        (1ull << 57), UINT64_MAX, UINT64_MAX - 1};
    for (uint64_t v : cases) {
        std::vector<uint8_t> buf;
        put_varint(buf, v);
        CHECK_EQ(buf.size(), varint_size(v));
        size_t used = 0;
        uint64_t back = get_varint(buf.data(), buf.size(), used);
        CHECK_EQ(back, v);
        CHECK_EQ(used, buf.size());
    }
    CHECK_EQ(varint_size(0), size_t(1));
    CHECK_EQ(varint_size(127), size_t(1));
    CHECK_EQ(varint_size(128), size_t(2));
    CHECK_EQ(varint_size(UINT64_MAX), size_t(9));
}

TEST(varint_truncated_errors) {
    std::vector<uint8_t> buf;
    put_varint(buf, UINT64_MAX);
    for (size_t cut = 1; cut < buf.size(); ++cut) {
        size_t used = 0;
        CHECK_DB_ERROR(get_varint(buf.data(), cut, used), int(Err::Storage), "truncated");
    }
}

// ---------------- records ----------------

TEST(record_roundtrip_basic) {
    std::vector<Value> in{Value::null(), Value::integer(0), Value::integer(-1),
                          Value::integer(42), Value::integer(INT64_MIN),
                          Value::integer(INT64_MAX), Value::text(""),
                          Value::text("hello"), Value::text("héllo"),
                          Value::text(std::string("with\0nul", 8))};
    auto bytes = encode_record(in);
    Record out = decode_record(bytes);
    CHECK_EQ(out.values.size(), in.size());
    for (size_t i = 0; i < in.size(); ++i)
        CHECK(out.values[i].same_as(in[i]));
}

TEST(record_minimal_int_encoding) {
    // serial types pick the smallest width; check by encoding distinct values
    // and confirming size differences
    auto enc = [](int64_t v) { return encode_record({Value::integer(v)}).size(); };
    CHECK_EQ(enc(0), enc(127));         // 0 and 127 both fit one byte
    CHECK(enc(127) < enc(128));        // 128 needs two
    CHECK(enc(INT64_MIN) > enc(0));
    CHECK_EQ(enc(5), enc(6));          // same width class
    CHECK_EQ(enc(1000), enc(2000));
}

TEST(record_long_strings) {
    std::string big(70000, 'x');       // forces multi-byte varint + big body
    std::vector<Value> in{Value::text(big), Value::integer(1), Value::null()};
    auto bytes = encode_record(in);
    Record out = decode_record(bytes);
    CHECK_EQ(out.values[0].as_text().size(), size_t(70000));
    CHECK_EQ(out.values[1].as_int(), int64_t(1));
    CHECK(out.values[2].is_null());
}

TEST(record_empty) {
    auto bytes = encode_record({});
    Record out = decode_record(bytes);
    CHECK_EQ(out.values.size(), size_t(0));
}

TEST(record_corruption_detected) {
    auto bytes = encode_record({Value::integer(7), Value::text("abc")});
    // truncated
    CHECK_DB_ERROR(decode_record(bytes.data(), bytes.size() - 1), int(Err::Storage),
                   "malformed record");
    // header size beyond the record
    auto bad = bytes;
    bad[0] = 0x7f;
    CHECK_DB_ERROR(decode_record(bad), int(Err::Storage), "malformed record");
    // unknown serial type (even number >= 12)
    auto bad2 = encode_record({Value::integer(1)});
    bad2[1] = 12;
    CHECK_DB_ERROR(decode_record(bad2), int(Err::Storage), "malformed record");
    // trailing bytes
    auto bad3 = bytes;
    bad3.push_back(0);
    CHECK_DB_ERROR(decode_record(bad3), int(Err::Storage), "trailing bytes");
    CHECK_DB_ERROR(decode_record(nullptr, 0), int(Err::Storage), "zero length");
}

// ---------------- key encodings ----------------

TEST(rowid_key_order_preserving) {
    int64_t cases[] = {INT64_MIN, INT64_MIN + 1, -2, -1, 0, 1, 2, 42,
                       INT64_MAX - 1, INT64_MAX};
    for (int64_t a : cases)
        for (int64_t b : cases) {
            std::vector<uint8_t> ea, eb;
            RowIdKey(a).encode(ea);
            RowIdKey(b).encode(eb);
            int want = a < b ? -1 : (a > b ? 1 : 0);
            int got = memcmp(ea.data(), eb.data(), 8) < 0
                          ? -1
                          : (memcmp(ea.data(), eb.data(), 8) > 0 ? 1 : 0);
            CHECK_EQ(got, want);
            CHECK_EQ(RowIdKey::decode(ea.data(), ea.size()).id, a);
        }
}

TEST(index_key_ordering) {
    IndexKey nul(Value::null(), 5), i5(Value::integer(5), 9), i5b(Value::integer(5), 3),
        im1(Value::integer(-1), 1), ta(Value::text("a"), 1), tb(Value::text("b"), 1),
        tab(Value::text("ab"), 1);
    CHECK(nul.compare(i5) < 0);       // NULL < INTEGER
    CHECK(i5.compare(ta) < 0);        // INTEGER < TEXT
    CHECK(im1.compare(i5) < 0);
    CHECK(i5b.compare(i5) < 0);       // same value: rowid breaks the tie
    CHECK(ta.compare(tb) < 0);
    CHECK(ta.compare(tab) < 0);       // text prefix first
}

TEST(index_key_roundtrip) {
    std::vector<IndexKey> keys{
        IndexKey(Value::null(), -3),
        IndexKey(Value::integer(INT64_MIN), 7),
        IndexKey(Value::integer(INT64_MAX), 7),
        IndexKey(Value::text(""), 0),
        IndexKey(Value::text("héllo wörld"), INT64_MAX),
        IndexKey(Value::text(std::string("a\0b", 3)), 44)};
    for (const IndexKey& k : keys) {
        std::vector<uint8_t> enc;
        k.encode(enc);
        size_t used = 0;
        IndexKey back = IndexKey::decode(enc.data(), enc.size(), used);
        CHECK_EQ(used, enc.size());
        CHECK(back.compare(k) == 0);
        CHECK(back.value.same_as(k.value));
        CHECK_EQ(back.rowid, k.rowid);
    }
}

TEST(index_key_malformed_detected) {
    size_t used = 0;
    CHECK_DB_ERROR(IndexKey::decode(nullptr, 0, used), int(Err::BTree), "empty");
    {
        std::vector<uint8_t> enc;
        IndexKey(Value::integer(1), 1).encode(enc);
        size_t used = 0;
        CHECK_DB_ERROR(IndexKey::decode(enc.data(), enc.size() - 1, used), int(Err::BTree),
                       "malformed index key");
    }
    {
        std::vector<uint8_t> bad{9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        size_t used = 0;
        CHECK_DB_ERROR(IndexKey::decode(bad.data(), bad.size(), used), int(Err::BTree),
                       "unknown tag 9");
    }
}
