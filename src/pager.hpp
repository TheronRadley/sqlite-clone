// pager.hpp — page-level storage management.
//
// The pager owns the database file and the page cache. Every byte that
// moves between memory and disk passes through here; no other module
// seeks in the file.
//
// Responsibilities:
//   * open/create the database file (validating the header, running
//     journal recovery if needed)
//   * read pages (through the cache), write pages (marking them dirty)
//   * allocate pages (via the freelist first, then file extension)
//   * track and flush dirty pages
//   * cache eviction (LRU over clean pages; dirty pages are never
//     evicted while a transaction is open — see TRANSACTIONS.md)
//   * the transaction state machine: begin/commit/rollback at the
//     transaction level, plus statement-level savepoints so a failed
//     statement inside a transaction does not leave partial state
//
// Invariants (checked in tests):
//   * page ids are stable: page N always lives at byte offset N*page_size
//   * reads of ids >= page_count fail loudly
//   * dirty pages are never silently dropped: they are flushed at commit
//     or reverted at rollback
//   * during a transaction the on-disk file only ever contains committed
//     page images (the cache absorbs all uncommitted writes)
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "error.hpp"

namespace sc {

class Pager;

namespace freelist {
uint32_t pop(Pager& pager);              // 0 = freelist empty
void push(Pager& pager, uint32_t id);
void set_fields(Pager& pager, uint32_t head, uint32_t count);
std::vector<uint32_t> collect(const Pager& pager);
void validate(const Pager& pager);
} // namespace freelist

// ---- database file header (page 0), see STORAGE_FORMAT.md ----
namespace hdr {
constexpr size_t MAGIC_OFF = 0;        // 8 bytes "SQLCLONE"
constexpr size_t VERSION_OFF = 8;      // u16
constexpr size_t PAGE_SIZE_OFF = 10;   // u16
constexpr size_t PAGE_COUNT_OFF = 12;  // u32
constexpr size_t FL_HEAD_OFF = 16;     // u32
constexpr size_t FL_COUNT_OFF = 20;    // u32
constexpr size_t CHANGE_OFF = 24;      // u32 change counter, bumped per commit
constexpr size_t CHECKSUM_OFF = 28;    // u64 FNV-1a over bytes 0..28
constexpr size_t HEADER_USED = 36;

struct HeaderInfo {
    uint32_t page_size, page_count, freelist_head, freelist_count, change_counter;
};

// Render a full header page image (rest of the page stays zero).
std::vector<uint8_t> make_image(uint32_t page_size, uint32_t page_count,
                                uint32_t fl_head, uint32_t fl_count,
                                uint32_t change_counter);
// Parse + validate magic/version/page-size/checksum. Throws StorageError.
HeaderInfo parse(const std::vector<uint8_t>& page0);
} // namespace hdr

struct PagerStats {
    uint64_t cache_hits = 0, cache_misses = 0;
    uint64_t disk_reads = 0, disk_writes = 0;
    uint64_t evictions = 0;
    uint64_t txn_page_writes = 0;   // pages modified inside transactions
};

class Pager {
public:
    // Open (or create) a file-backed database. If a hot rollback journal
    // exists, recovery runs first.
    static std::unique_ptr<Pager> open_file(const std::string& path,
                                            uint32_t page_size = 4096);
    // In-memory database (same code paths; no journal file, no fsync).
    static std::unique_ptr<Pager> open_memory(uint32_t page_size = 4096);

    ~Pager();  // does NOT flush: dropping a Pager with uncommitted dirty
               // pages discards them (equivalent to a crash). Callers that
               // care use commit/rollback first.

    uint32_t page_size() const { return page_size_; }
    uint32_t page_count() const { return page_count_; }
    bool page_exists(uint32_t id) const { return id < page_count_; }
    bool is_file() const;
    const std::string& path() const;
    const PagerStats& stats() const { return stats_; }
    size_t cached_pages() const { return cache_.size(); }
    size_t dirty_pages() const;
    void set_cache_capacity(size_t pages) { cache_capacity_ = pages < 8 ? 8 : pages; }
    size_t cache_capacity() const { return cache_capacity_; }

    // ---- page access ----
    // Returns a *copy* of the page bytes. Pages are small and the copy
    // makes ownership explicit; callers mutate and call write_page.
    std::vector<uint8_t> get_page(uint32_t id);
    // Store a modified page and mark it dirty. This is the single write
    // path and the journal hook point.
    void write_page(uint32_t id, std::vector<uint8_t> data);
    // Mark an already-cached page dirty without changing its bytes.
    void mark_dirty(uint32_t id);
    bool is_dirty(uint32_t id) const;

    // ---- allocation ----
    uint32_t allocate_page();
    void free_page(uint32_t id);

    // ---- flushing ----
    void flush_page(uint32_t id);
    void flush_all();   // writes all dirty pages and fsyncs
    void sync();

    // ---- transactions ----
    void begin_txn();
    bool in_txn() const { return txn_active_; }
    bool stmt_active() const { return stmt_active_; }
    void begin_stmt();        // requires an open transaction
    void rollback_stmt();
    void end_stmt();
    void commit_txn();        // journal -> flush -> fsync -> journal removed
    void rollback_txn();

    // Crash simulation for tests: abandon everything without cleanup.
    void abandon() { abandoned_ = true; }

    // Direct store access for recovery/rollback (bypasses the cache).
    std::vector<uint8_t> store_read(uint32_t id);
    void store_write(uint32_t id, const std::vector<uint8_t>& data);

    // Abstract byte store (file or memory); public so the .cpp can
    // implement FileStore/MemStore outside the class.
    struct PageStore {
        virtual ~PageStore() = default;
        virtual std::vector<uint8_t> read(uint32_t id, uint32_t page_size) = 0;
        virtual void write(uint32_t id, const uint8_t* data, uint32_t page_size) = 0;
        virtual void truncate_to(uint32_t pages, uint32_t page_size) = 0;
        virtual uint64_t size_bytes() const = 0;
        virtual void sync() = 0;
        virtual std::vector<uint8_t> read_prefix(size_t n) = 0;
    };

private:
    Pager() = default;
    struct Entry {
        std::vector<uint8_t> data;
        bool dirty = false;
        uint64_t tick = 0;
    };

    void maybe_evict();
    std::vector<uint8_t> current_image(uint32_t id);  // cache copy or store read

    std::unique_ptr<PageStore> store_;
    std::string path_;
    bool file_backed_ = false;
    uint32_t page_size_ = 4096;
    uint32_t page_count_ = 0;

    std::unordered_map<uint32_t, Entry> cache_;
    size_t cache_capacity_ = 512;
    uint64_t clock_ = 0;
    PagerStats stats_;

    bool txn_active_ = false;
    uint32_t original_page_count_ = 0;
    std::map<uint32_t, std::vector<uint8_t>> txn_orig_;   // committed images

    bool stmt_active_ = false;
    uint32_t stmt_page_count_ = 0;
    std::map<uint32_t, std::vector<uint8_t>> stmt_images_;

    bool abandoned_ = false;
};

} // namespace sc
