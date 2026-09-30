// journal.hpp — the rollback journal file + crash recovery.
//
// Protocol (documented in TRANSACTIONS.md):
//
//   1. During a transaction the pager keeps *original* (committed) images
//      of every page it modifies in memory. Nothing dirty is written to
//      the database file while the transaction is open.
//   2. COMMIT first writes the journal file (original images + the page
//      count at transaction start), fsyncs it, then flushes dirty pages
//      to the database file and fsyncs that, then deletes the journal.
//      The commit point is the journal deletion.
//   3. OPEN: if a journal file exists, the database is in an unknown
//      mid-commit state. The journal is validated (magic + checksum) and
//      replayed backwards: original images are restored and the file is
//      truncated to the original page count. A corrupt journal is a hard
//      error — we never guess about disk state.
//
// Journal file layout (see STORAGE_FORMAT.md):
//   0..8    magic "SQLCJRN1"
//   8..10   journal format version (u16 BE, currently 1)
//   10..12  reserved (u16 BE, 0)
//   12..16  page size (u32 BE)
//   16..20  original page count (u32 BE)
//   20..24  record count (u32 BE)
//   24..32  FNV-1a 64 checksum of bytes 12..24 plus all record bytes (u64 BE)
//   32..    records: (u32 page id, page image) * record count
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sc {

struct JournalData {
    uint32_t page_size = 0;
    uint32_t original_page_count = 0;
    std::map<uint32_t, std::vector<uint8_t>> pages;  // page id -> original image
};

namespace journal {

std::string journal_path_for(const std::string& db_path);
bool exists(const std::string& db_path);

// Write the journal file and fsync it.
void write(const std::string& db_path, const JournalData& data);

// Remove the journal file (after a successful commit or rollback).
void remove(const std::string& db_path);

// Read + validate. Throws StorageError on any inconsistency.
JournalData read_and_validate(const std::string& db_path);

// If a journal exists: validate, restore original images into the database
// file, truncate to the original page count, fsync, remove the journal.
// Returns true if recovery happened. Throws StorageError on a corrupt
// journal or a missing database file.
bool recover_if_present(const std::string& db_path);

} // namespace journal
} // namespace sc
