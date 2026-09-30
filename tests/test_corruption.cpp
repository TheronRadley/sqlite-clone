// test_corruption.cpp — malformed disk data must produce typed errors,
// never crashes, hangs, or garbage results. Every test mutates raw file
// bytes and then expects a structured DbError from a well-defined entry
// point (open, execute, or validate).
#include "test_util.hpp"

#include <cstdio>

#include "database.hpp"
#include "journal.hpp"
#include "pager.hpp"
#include "util.hpp"

using namespace sc;

namespace {

// build a database with one populated table (+ index) and return its path
std::string make_db() {
    std::string path = test::temp_db_path();
    auto db = Database::open(path);
    (void)db->execute("CREATE TABLE t (id INTEGER PRIMARY KEY, s TEXT, n INTEGER)");
    for (int i = 1; i <= 120; ++i)
        (void)db->execute("INSERT INTO t VALUES (" + std::to_string(i) + ", 'row" +
                          std::to_string(i) + "', " + std::to_string(i * 3) + ")");
    (void)db->execute("CREATE INDEX t_n ON t(n)");
    return path;
}

// build a database whose table tree has an interior root (enough rows)
std::string make_big_db() {
    std::string path = test::temp_db_path();
    auto db = Database::open(path);
    (void)db->execute("CREATE TABLE big (id INTEGER PRIMARY KEY, s TEXT)");
    for (int i = 1; i <= 900; ++i)
        (void)db->execute("INSERT INTO big VALUES (" + std::to_string(i) + ", 's" +
                          std::to_string(i % 97) + "')");
    return path;
}

void poke(const std::string& path, uint64_t offset, const void* data, size_t n) {
    std::FILE* f = fopen(path.c_str(), "r+b");
    CHECK(f != nullptr);
    CHECK(fseek(f, long(offset), SEEK_SET) == 0);
    CHECK(fwrite(data, 1, n, f) == n);
    fclose(f);
}

void poke_byte(const std::string& path, uint64_t offset, uint8_t v) {
    poke(path, offset, &v, 1);
}

// rewrite a u32 big-endian and keep the header checksum honest
void poke_header_u32(const std::string& path, size_t field_off, uint32_t v) {
    uint8_t page[4096];
    std::FILE* f = fopen(path.c_str(), "rb");
    CHECK(fread(page, 1, sizeof page, f) == sizeof page);
    fclose(f);
    wr_be32(page + field_off, v);
    wr_be64(page + 28, fnv1a64(page, 28));   // checksum covers bytes 0..28
    poke(path, 0, page, sizeof page);
}

} // namespace

TEST(corrupt_bad_magic_rejected_at_open) {
    std::string path = make_db();
    poke_byte(path, 0, 'X');
    CHECK_DB_ERROR(Database::open(path), int(Err::Storage), "bad magic");
    std::remove(path.c_str());
}

TEST(corrupt_header_checksum_rejected_at_open) {
    std::string path = make_db();
    poke_byte(path, 12, 0xEE);   // page count field: inside the checksummed area
    CHECK_DB_ERROR(Database::open(path), int(Err::Storage), "checksum mismatch");
    std::remove(path.c_str());
}

TEST(corrupt_impossible_page_size_rejected) {
    std::string path = make_db();
    poke_header_u32(path, hdr::PAGE_SIZE_OFF, 3000);   // not a power of two
    CHECK_DB_ERROR(Database::open(path), int(Err::Storage), "invalid page size");
    std::remove(path.c_str());
}

TEST(corrupt_truncated_file_rejected) {
    std::string path = make_db();
    {
        std::FILE* f = fopen(path.c_str(), "rb");
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fclose(f);
        CHECK(::truncate(path.c_str(), size - 4096) == 0);
    }
    CHECK_DB_ERROR(Database::open(path), int(Err::Storage), "does not match its header");
    std::remove(path.c_str());
}

TEST(corrupt_page_type_byte) {
    std::string path = make_db();
    poke_byte(path, uint64_t(2) * 4096, 99);   // table root page, impossible type
    auto db = Database::open(path);            // header is fine; open succeeds
    CHECK_DB_ERROR(db->execute("SELECT * FROM t"), int(Err::BTree),
                   "invalid page type byte 99");
    CHECK_DB_ERROR(db->validate(false), int(Err::BTree), "invalid page type");
    std::remove(path.c_str());
}

TEST(corrupt_cell_count_overflow) {
    std::string path = make_db();
    uint8_t big[2] = {0xff, 0xff};
    poke(path, uint64_t(2) * 4096 + 2, big, 2);   // cell count = 65535
    auto db = Database::open(path);
    CHECK_DB_ERROR(db->validate(false), int(Err::BTree), "overlaps content start");
    std::remove(path.c_str());
}

TEST(corrupt_content_start_one) {
    std::string path = make_db();
    uint8_t one[2] = {0x00, 0x01};
    poke(path, uint64_t(2) * 4096 + 4, one, 2);   // content start = 1
    auto db = Database::open(path);
    CHECK_DB_ERROR(db->validate(false), int(Err::BTree), "content start");
    std::remove(path.c_str());
}

TEST(corrupt_content_start_beyond_page) {
    std::string path = make_db();
    uint8_t big[2] = {0x20, 0x00};   // 8192 > 4096
    poke(path, uint64_t(2) * 4096 + 4, big, 2);
    auto db = Database::open(path);
    CHECK_DB_ERROR(db->validate(false), int(Err::BTree), "beyond page size");
    std::remove(path.c_str());
}

TEST(corrupt_child_pointer_out_of_file) {
    std::string path = make_big_db();
    // page 2 is an interior root; its right-most child pointer lives at
    // offset 8 of the page
    uint8_t big[4] = {0x00, 0x01, 0x86, 0x9f};   // 99999
    poke(path, uint64_t(2) * 4096 + 8, big, 4);
    auto db = Database::open(path);
    // execution reaches the bad pointer through the pager...
    CHECK_DB_ERROR(db->execute("SELECT COUNT(*) FROM big"), int(Err::Pager),
                   "out of range");
    // ...while the validator reports it in tree terms
    CHECK_DB_ERROR(db->validate(false), int(Err::BTree), "beyond the end of the file");
    std::remove(path.c_str());
}

TEST(corrupt_impossible_payload_length) {
    std::string path = make_db();
    // first cell of the table root leaf: pointer array entry 0 at page+8
    uint8_t ptr[2];
    {
        std::FILE* f = fopen(path.c_str(), "rb");
        CHECK(fseek(f, long(2 * 4096 + 8), SEEK_SET) == 0);
        CHECK(fread(ptr, 1, 2, f) == 2);
        fclose(f);
    }
    uint16_t cell_off = uint16_t((ptr[0] << 8) | ptr[1]);
    // cell = varint(payload_len) key(8) payload...: declare 1 GiB
    uint8_t huge[5] = {0xff, 0xff, 0xff, 0xff, 0x7f};
    poke(path, uint64_t(2) * 4096 + cell_off, huge, 5);
    auto db = Database::open(path);
    CHECK_DB_ERROR(db->execute("SELECT * FROM t"), int(Err::BTree),
                   "impossible payload length");
    std::remove(path.c_str());
}

TEST(corrupt_record_serial_type) {
    std::string path = make_db();
    // first cell's payload starts after varint(len) + key(8): overwrite the
    // record's first header byte (a serial type) with an unknown value
    uint8_t ptr[2];
    {
        std::FILE* f = fopen(path.c_str(), "rb");
        CHECK(fseek(f, long(2 * 4096 + 8), SEEK_SET) == 0);
        CHECK(fread(ptr, 1, 2, f) == 2);
        fclose(f);
    }
    uint16_t cell_off = uint16_t((ptr[0] << 8) | ptr[1]);
    // cell = varint(payload_len) key(8) payload; payload = varint(header_size)
    // followed by the serial types. Overwrite the first serial type.
    size_t first_serial = size_t(cell_off) + 1 + 8 + 1;
    poke_byte(path, uint64_t(2) * 4096 + first_serial, 9);   // serial type 9: unused
    auto db = Database::open(path);
    CHECK_DB_ERROR(db->execute("SELECT * FROM t"), int(Err::Storage),
                   "unknown serial type 9");
    std::remove(path.c_str());
}

TEST(corrupt_freelist_head_not_a_trunk) {
    std::string path = make_db();
    // point the freelist at the table's root page (a leaf, not a trunk):
    // any allocation that pops the freelist must fail loudly
    poke_header_u32(path, hdr::FL_HEAD_OFF, 2);
    poke_header_u32(path, hdr::FL_COUNT_OFF, 1);
    auto db = Database::open(path);
    // an insert big enough to spill into overflow pages must allocate, and
    // allocation pops the (corrupt) freelist
    std::string big(6000, 'x');
    CHECK_DB_ERROR(db->execute("INSERT INTO t VALUES (999, '" + big + "', 1)"),
                   int(Err::Pager), "not a trunk page");
    // the freelist is genuinely corrupt: validation reports it too (the
    // failed insert added nothing on top)
    CHECK_DB_ERROR(db->validate(false), int(Err::Pager), "not a trunk page");
    std::remove(path.c_str());
}

TEST(corrupt_page_flag_byte) {
    std::string path = make_db();
    poke_byte(path, uint64_t(2) * 4096 + 1, 7);   // flags must be zero
    auto db = Database::open(path);
    CHECK_DB_ERROR(db->validate(false), int(Err::BTree), "flag byte");
    std::remove(path.c_str());
}

TEST(corrupt_error_never_leaves_open_txn) {
    // after a corruption error the database object stays usable for reads
    // that do not touch the damaged structure, and never reports an open
    // transaction
    std::string path = make_db();
    (void)Database::open(path);
    poke_byte(path, uint64_t(2) * 4096, 99);
    auto db = Database::open(path);
    CHECK_DB_ERROR(db->execute("SELECT * FROM t"), int(Err::BTree),
                   "invalid page type byte 99");
    CHECK(!db->in_txn());
    CHECK_NO_ERROR(db->execute("SELECT 1 + 1"));   // catalog still readable
    std::remove(path.c_str());
}
