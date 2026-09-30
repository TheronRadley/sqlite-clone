#include "pager.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "journal.hpp"
#include "page.hpp"
#include "util.hpp"

namespace sc {

// =====================================================================
// Page stores
// =====================================================================

namespace {

class FileStore : public Pager::PageStore {
public:
    explicit FileStore(const std::string& path, bool create) : path_(path) {
        f_ = std::fopen(path.c_str(), create ? "w+b" : "r+b");
        if (!f_)
            throw DbError::storage(str("cannot open database file ", path, ": ",
                                       std::strerror(errno)));
    }
    ~FileStore() override {
        if (f_) std::fclose(f_);
    }
    FileStore(const FileStore&) = delete;
    FileStore& operator=(const FileStore&) = delete;

    std::vector<uint8_t> read(uint32_t id, uint32_t page_size) override {
        if (std::fseek(f_, long(id) * long(page_size), SEEK_SET) != 0)
            throw DbError::pager(str("cannot seek to page ", id));
        std::vector<uint8_t> buf(page_size);
        size_t got = std::fread(buf.data(), 1, page_size, f_);
        if (got != page_size)
            throw DbError::pager(str("short read on page ", id, " (got ", got, " of ",
                                     page_size, " bytes)"));
        return buf;
    }
    void write(uint32_t id, const uint8_t* data, uint32_t page_size) override {
        if (std::fseek(f_, long(id) * long(page_size), SEEK_SET) != 0)
            throw DbError::pager(str("cannot seek to page ", id));
        if (std::fwrite(data, 1, page_size, f_) != page_size)
            throw DbError::pager(str("cannot write page ", id));
    }
    void truncate_to(uint32_t pages, uint32_t page_size) override {
        if (::ftruncate(::fileno(f_), off_t(pages) * off_t(page_size)) != 0)
            throw DbError::pager("cannot truncate database file");
    }
    uint64_t size_bytes() const override {
        if (std::fseek(f_, 0, SEEK_END) != 0) return 0;
        return uint64_t(std::ftell(f_));
    }
    void sync() override {
        if (std::fflush(f_) != 0)
            throw DbError::pager("cannot flush database file");
        if (::fsync(::fileno(f_)) != 0)
            throw DbError::pager(str("cannot fsync database file: ", std::strerror(errno)));
    }
    std::vector<uint8_t> read_prefix(size_t n) override {
        if (std::fseek(f_, 0, SEEK_SET) != 0)
            throw DbError::pager("cannot seek to start of database file");
        std::vector<uint8_t> buf(n);
        size_t got = std::fread(buf.data(), 1, n, f_);
        buf.resize(got);
        return buf;
    }

private:
    std::string path_;
    std::FILE* f_ = nullptr;
};

class MemStore : public Pager::PageStore {
public:
    std::vector<uint8_t> read(uint32_t id, uint32_t page_size) override {
        size_t off = size_t(id) * page_size;
        if (off + page_size > blob_.size())
            throw DbError::pager(str("page ", id, " has never been written (beyond end)"));
        return std::vector<uint8_t>(blob_.begin() + off, blob_.begin() + off + page_size);
    }
    void write(uint32_t id, const uint8_t* data, uint32_t page_size) override {
        size_t off = size_t(id) * page_size;
        if (off + page_size > blob_.size()) blob_.resize(off + page_size, 0);
        std::copy(data, data + page_size, blob_.begin() + off);
    }
    void truncate_to(uint32_t pages, uint32_t page_size) override {
        blob_.resize(size_t(pages) * page_size);
    }
    uint64_t size_bytes() const override { return blob_.size(); }
    void sync() override {}
    std::vector<uint8_t> read_prefix(size_t n) override {
        size_t got = std::min(n, blob_.size());
        return std::vector<uint8_t>(blob_.begin(), blob_.begin() + got);
    }

private:
    std::vector<uint8_t> blob_;
};

bool is_power_of_two(uint32_t v) { return v && (v & (v - 1)) == 0; }

void check_page_size(uint32_t ps) {
    if (ps < 512 || ps > 32768 || !is_power_of_two(ps))
        throw DbError::pager(str("invalid page size ", ps, " (must be a power of two in 512..32768)"));
}

} // namespace

// =====================================================================
// File header
// =====================================================================

namespace hdr {

static const char MAGIC[8] = {'S', 'Q', 'L', 'C', 'L', 'O', 'N', 'E'};

std::vector<uint8_t> make_image(uint32_t page_size, uint32_t page_count,
                                uint32_t fl_head, uint32_t fl_count,
                                uint32_t change_counter) {
    std::vector<uint8_t> p(page_size, 0);
    std::copy(MAGIC, MAGIC + 8, p.begin());
    wr_be16(p.data() + VERSION_OFF, 1);
    wr_be16(p.data() + PAGE_SIZE_OFF, uint16_t(page_size));
    wr_be32(p.data() + PAGE_COUNT_OFF, page_count);
    wr_be32(p.data() + FL_HEAD_OFF, fl_head);
    wr_be32(p.data() + FL_COUNT_OFF, fl_count);
    wr_be32(p.data() + CHANGE_OFF, change_counter);
    wr_be64(p.data() + CHECKSUM_OFF, fnv1a64(p.data(), 28));
    return p;
}

HeaderInfo parse(const std::vector<uint8_t>& p0) {
    if (p0.size() < HEADER_USED)
        throw DbError::storage("database header truncated");
    for (int i = 0; i < 8; ++i)
        if (p0[size_t(i)] != uint8_t(MAGIC[i]))
            throw DbError::storage("file is not a sqlite-clone database (bad magic)");
    uint16_t version = rd_be16(p0.data() + VERSION_OFF);
    if (version != 1)
        throw DbError::storage(str("unsupported database format version ", version));
    uint64_t sum = rd_be64(p0.data() + CHECKSUM_OFF);
    if (sum != fnv1a64(p0.data(), 28))
        throw DbError::storage("database header checksum mismatch: header is corrupt");
    HeaderInfo h;
    h.page_size = rd_be16(p0.data() + PAGE_SIZE_OFF);
    h.page_count = rd_be32(p0.data() + PAGE_COUNT_OFF);
    h.freelist_head = rd_be32(p0.data() + FL_HEAD_OFF);
    h.freelist_count = rd_be32(p0.data() + FL_COUNT_OFF);
    h.change_counter = rd_be32(p0.data() + CHANGE_OFF);
    if (!is_power_of_two(h.page_size) || h.page_size < 512 || h.page_size > 32768)
        throw DbError::storage(str("invalid page size ", h.page_size, " in database header"));
    return h;
}

} // namespace hdr

// =====================================================================
// Freelist
// =====================================================================

namespace freelist {

// trunk page layout: [0]=type 5, [4..8] next trunk, [8..12] leaf count,
// [12..] leaf page ids (u32 each)
static constexpr size_t TRUNK_NEXT_OFF = 4;
static constexpr size_t TRUNK_COUNT_OFF = 8;
static constexpr size_t TRUNK_LEAVES_OFF = 12;

static uint32_t trunk_capacity(uint32_t page_size) {
    return (page_size - TRUNK_LEAVES_OFF) / 4;
}

void set_fields(Pager& pager, uint32_t head, uint32_t count) {
    auto p0 = pager.get_page(0);
    wr_be32(p0.data() + hdr::FL_HEAD_OFF, head);
    wr_be32(p0.data() + hdr::FL_COUNT_OFF, count);
    // keep the checksum honest: recompute over bytes 0..28
    wr_be64(p0.data() + hdr::CHECKSUM_OFF, fnv1a64(p0.data(), 28));
    pager.write_page(0, std::move(p0));
}

struct Fields {
    uint32_t head = 0, count = 0;
};

static Fields read_fields(Pager& pager) {
    auto p0 = pager.get_page(0);
    hdr::HeaderInfo h = hdr::parse(p0);  // validates magic + checksum
    Fields f;
    f.head = h.freelist_head;
    f.count = h.freelist_count;
    return f;
}

static void check_trunk_page(const std::vector<uint8_t>& page, uint32_t id,
                             uint32_t page_count) {
    if (page.size() < TRUNK_LEAVES_OFF)
        throw DbError::pager(str("freelist trunk page ", id, " is too small"));
    if (page[0] != uint8_t(PageType::FreelistTrunk))
        throw DbError::pager(str("freelist trunk pointer targets page ", id,
                                 " which is not a trunk page (type ", int(page[0]), ")"));
    uint32_t next = rd_be32(page.data() + TRUNK_NEXT_OFF);
    uint32_t n = rd_be32(page.data() + TRUNK_COUNT_OFF);
    if (next >= page_count && next != 0)
        throw DbError::pager(str("freelist trunk ", id, " points to page ", next,
                                 " beyond the end of the file"));
    if (n > trunk_capacity(uint32_t(page.size())))
        throw DbError::pager(str("freelist trunk ", id, " claims ", n,
                                 " leaf pages (max ", trunk_capacity(uint32_t(page.size())), ")"));
}

uint32_t pop(Pager& pager) {
    Fields f = read_fields(pager);
    if (f.head == 0) return 0;

    auto trunk = pager.get_page(f.head);
    check_trunk_page(trunk, f.head, pager.page_count());

    uint32_t next = rd_be32(trunk.data() + TRUNK_NEXT_OFF);
    uint32_t n = rd_be32(trunk.data() + TRUNK_COUNT_OFF);

    uint32_t got;
    if (n > 0) {
        got = rd_be32(trunk.data() + TRUNK_LEAVES_OFF + 4 * (n - 1));
        if (got >= pager.page_count() || got < 2)
            throw DbError::pager(str("freelist references invalid page ", got));
        wr_be32(trunk.data() + TRUNK_COUNT_OFF, n - 1);
        pager.write_page(f.head, std::move(trunk));
        set_fields(pager, f.head, f.count - 1);
    } else {
        got = f.head;  // the trunk itself becomes free for reuse
        set_fields(pager, next, f.count - 1);
    }
    return got;
}

void push(Pager& pager, uint32_t id) {
    if (id < 2)
        throw DbError::pager(str("page ", id, " is reserved (header/catalog) and cannot be freed"));
    if (id >= pager.page_count())
        throw DbError::pager(str("cannot free page ", id, ": beyond page count"));

    Fields f = read_fields(pager);
    if (f.head == 0) {
        std::vector<uint8_t> page(pager.page_size(), 0);
        page[0] = uint8_t(PageType::FreelistTrunk);
        wr_be32(page.data() + TRUNK_NEXT_OFF, 0);
        wr_be32(page.data() + TRUNK_COUNT_OFF, 0);
        pager.write_page(id, std::move(page));
        set_fields(pager, id, f.count + 1);
        return;
    }

    auto trunk = pager.get_page(f.head);
    check_trunk_page(trunk, f.head, pager.page_count());
    uint32_t n = rd_be32(trunk.data() + TRUNK_COUNT_OFF);

    if (n < trunk_capacity(pager.page_size())) {
        wr_be32(trunk.data() + TRUNK_LEAVES_OFF + 4 * n, id);
        wr_be32(trunk.data() + TRUNK_COUNT_OFF, n + 1);
        pager.write_page(f.head, std::move(trunk));
        set_fields(pager, f.head, f.count + 1);
    } else {
        // current trunk is full: the freed page becomes the new trunk
        std::vector<uint8_t> page(pager.page_size(), 0);
        page[0] = uint8_t(PageType::FreelistTrunk);
        wr_be32(page.data() + TRUNK_NEXT_OFF, f.head);
        wr_be32(page.data() + TRUNK_COUNT_OFF, 0);
        pager.write_page(id, std::move(page));
        set_fields(pager, id, f.count + 1);
    }
}

std::vector<uint32_t> collect(const Pager& pager) {
    // The freelist walker needs to read pages; const_cast is contained here
    // and the reads have no side effects (page 0 is always cached/rewritten
    // identically). This is the only const_cast in the engine.
    Pager& p = const_cast<Pager&>(pager);
    std::vector<uint32_t> out;
    uint32_t head = 0;
    {
        auto p0 = p.get_page(0);
        hdr::HeaderInfo h = hdr::parse(p0);
        head = h.freelist_head;
    }
    uint32_t seen_trunks = 0;
    while (head != 0) {
        if (++seen_trunks > pager.page_count())
            throw DbError::pager("freelist contains a cycle");
        auto trunk = p.get_page(head);
        check_trunk_page(trunk, head, pager.page_count());
        uint32_t n = rd_be32(trunk.data() + TRUNK_COUNT_OFF);
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t leaf = rd_be32(trunk.data() + TRUNK_LEAVES_OFF + 4 * i);
            if (leaf >= pager.page_count() || leaf < 2)
                throw DbError::pager(str("freelist references invalid page ", leaf));
            out.push_back(leaf);
        }
        out.push_back(head);
        head = rd_be32(trunk.data() + TRUNK_NEXT_OFF);
    }
    return out;
}

void validate(const Pager& pager) {
    auto p0 = const_cast<Pager&>(pager).get_page(0);
    hdr::HeaderInfo h = hdr::parse(p0);
    std::vector<uint32_t> pages = collect(pager);
    if (pages.size() != h.freelist_count)
        throw DbError::pager(str("freelist page count mismatch: header says ",
                                 h.freelist_count, ", walk found ", pages.size()));
    std::vector<uint32_t> sorted = pages;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 1; i < sorted.size(); ++i)
        if (sorted[i] == sorted[i - 1])
            throw DbError::pager(str("page ", sorted[i], " appears twice in the freelist"));
}

} // namespace freelist

// =====================================================================
// Pager
// =====================================================================

Pager::~Pager() {
    // Intentionally does not flush. Dropping uncommitted state == crash.
    (void)abandoned_;
}

bool Pager::is_file() const { return file_backed_; }
const std::string& Pager::path() const { return path_; }

size_t Pager::dirty_pages() const {
    size_t n = 0;
    for (const auto& [id, e] : cache_)
        if (e.dirty) ++n;
    return n;
}

std::vector<uint8_t> Pager::current_image(uint32_t id) {
    auto it = cache_.find(id);
    if (it != cache_.end()) return it->second.data;
    return store_->read(id, page_size_);
}

std::vector<uint8_t> Pager::get_page(uint32_t id) {
    if (id >= page_count_)
        throw DbError::pager(str("page ", id, " is out of range (database has ",
                                 page_count_, " pages)"));
    auto it = cache_.find(id);
    if (it != cache_.end()) {
        ++stats_.cache_hits;
        it->second.tick = ++clock_;
        std::vector<uint8_t> data = it->second.data;
        // enforce the capacity on hits too: a transaction may have
        // legitimately overshot it, and without this check the cache would
        // stay oversized until the next miss
        maybe_evict();
        return data;
    }
    ++stats_.cache_misses;
    ++stats_.disk_reads;
    std::vector<uint8_t> data = store_->read(id, page_size_);
    cache_[id] = Entry{data, false, ++clock_};
    maybe_evict();
    return data;
}

void Pager::write_page(uint32_t id, std::vector<uint8_t> data) {
    if (id >= page_count_)
        throw DbError::pager(str("cannot write page ", id, ": out of range (database has ",
                                 page_count_, " pages)"));
    if (data.size() != page_size_)
        throw DbError::pager(str("page ", id, ": write of ", data.size(),
                                 " bytes (page size is ", page_size_, ")"));

    // Journal hooks (order matters: statement images see the pre-write
    // state which may already include earlier changes from this transaction).
    if (stmt_active_ && stmt_images_.find(id) == stmt_images_.end())
        stmt_images_[id] = current_image(id);
    if (txn_active_ && txn_orig_.find(id) == txn_orig_.end()) {
        if (id < original_page_count_) {
            // the committed image: read straight from the store, the cache
            // may already hold this transaction's dirty version
            txn_orig_[id] = store_->read(id, page_size_);
            ++stats_.txn_page_writes;
        }
        // pages beyond original_page_count_ did not exist; rollback
        // truncates them away
    }

    Entry& e = cache_[id];
    e.data = std::move(data);
    e.dirty = true;
    e.tick = ++clock_;
    maybe_evict();
}

void Pager::mark_dirty(uint32_t id) {
    auto it = cache_.find(id);
    if (it == cache_.end()) {
        if (id >= page_count_)
            throw DbError::pager(str("page ", id, " is out of range"));
        ++stats_.disk_reads;
        cache_[id] = Entry{store_->read(id, page_size_), true, ++clock_};
    } else {
        it->second.dirty = true;
        it->second.tick = ++clock_;
    }
}

bool Pager::is_dirty(uint32_t id) const {
    auto it = cache_.find(id);
    return it != cache_.end() && it->second.dirty;
}

void Pager::maybe_evict() {
    while (cache_.size() > cache_capacity_) {
        // try to evict the least recently used CLEAN page
        bool found = false;
        uint32_t victim = 0;
        uint64_t best = UINT64_MAX;
        for (const auto& [id, e] : cache_) {
            if (!e.dirty && e.tick < best) {
                best = e.tick;
                victim = id;
                found = true;
            }
        }
        if (found) {
            cache_.erase(victim);
            ++stats_.evictions;
            continue;
        }
        if (txn_active_) {
            // Never spill dirty pages mid-transaction: the on-disk file must
            // keep containing only committed images until the journal is in
            // place. The cache may transiently exceed its capacity.
            break;
        }
        // no transaction: flush + drop the least recently used dirty page
        bool dfound = false;
        uint32_t dvictim = 0;
        uint64_t dbest = UINT64_MAX;
        for (const auto& [id, e] : cache_) {
            if (e.dirty && e.tick < dbest) {
                dbest = e.tick;
                dvictim = id;
                dfound = true;
            }
        }
        if (!dfound) break;  // nothing evictable (cannot happen)
        flush_page(dvictim);
        cache_.erase(dvictim);
        ++stats_.evictions;
    }
}

uint32_t Pager::allocate_page() {
    uint32_t id = freelist::pop(*this);
    if (id != 0) {
        // route through write_page so journal snapshots capture the page's
        // pre-allocation image (important when a trunk page is reused)
        std::vector<uint8_t> zeros(page_size_, 0);
        write_page(id, std::move(zeros));
        return id;
    }
    id = page_count_++;
    cache_[id] = Entry{std::vector<uint8_t>(page_size_, 0), true, ++clock_};
    return id;
}

void Pager::free_page(uint32_t id) {
    freelist::push(*this, id);
}

void Pager::flush_page(uint32_t id) {
    auto it = cache_.find(id);
    if (it == cache_.end() || !it->second.dirty) return;
    store_->write(id, it->second.data.data(), page_size_);
    it->second.dirty = false;
    ++stats_.disk_writes;
}

void Pager::flush_all() {
    for (auto& [id, e] : cache_)
        if (e.dirty) {
            store_->write(id, e.data.data(), page_size_);
            e.dirty = false;
            ++stats_.disk_writes;
        }
    store_->sync();
}

void Pager::sync() { store_->sync(); }

std::vector<uint8_t> Pager::store_read(uint32_t id) {
    return store_->read(id, page_size_);
}

void Pager::store_write(uint32_t id, const std::vector<uint8_t>& data) {
    store_->write(id, data.data(), page_size_);
}

// ---- transactions ----

void Pager::begin_txn() {
    if (txn_active_)
        throw DbError::txn("cannot start a transaction within a transaction");
    for (const auto& [id, e] : cache_)
        if (e.dirty)
            throw DbError::pager("internal error: dirty pages outside a transaction");
    txn_active_ = true;
    original_page_count_ = page_count_;
    txn_orig_.clear();
}

void Pager::begin_stmt() {
    if (!txn_active_)
        throw DbError::txn("internal error: statement savepoint outside a transaction");
    if (stmt_active_)
        throw DbError::txn("internal error: statement savepoint already active");
    stmt_active_ = true;
    stmt_page_count_ = page_count_;
    stmt_images_.clear();
}

void Pager::rollback_stmt() {
    if (!stmt_active_)
        throw DbError::txn("internal error: no statement savepoint to roll back");
    for (const auto& [id, img] : stmt_images_) {
        Entry& e = cache_[id];
        e.data = img;
        e.dirty = true;
        e.tick = ++clock_;
    }
    if (page_count_ > stmt_page_count_) {
        // pages allocated by the failed statement disappear
        for (auto it = cache_.begin(); it != cache_.end();) {
            if (it->first >= stmt_page_count_) it = cache_.erase(it);
            else ++it;
        }
        page_count_ = stmt_page_count_;
    }
    stmt_images_.clear();
    stmt_active_ = false;
}

void Pager::end_stmt() {
    if (!stmt_active_)
        throw DbError::txn("internal error: no statement savepoint to end");
    stmt_images_.clear();
    stmt_active_ = false;
}

void Pager::commit_txn() {
    if (!txn_active_)
        throw DbError::txn("cannot commit: no transaction is active");
    if (stmt_active_) end_stmt();

    // A transaction is a no-op only when it changed nothing at all: no
    // existing page was modified, nothing was allocated, nothing is dirty.
    // (A txn that only *extends* the file still must refresh the header
    // and flush, or the extension would be silently lost.)
    bool grew = page_count_ != original_page_count_;
    if (txn_orig_.empty() && !grew && dirty_pages() == 0) {
        // read-only transaction: nothing to write
        txn_active_ = false;
        return;
    }

    // 1. refresh the file header (page count, change counter) in the cache
    {
        auto p0 = get_page(0);
        hdr::HeaderInfo h = hdr::parse(p0);
        auto fresh = hdr::make_image(page_size_, page_count_, h.freelist_head,
                                     h.freelist_count, h.change_counter + 1);
        // preserve the catalog-root invariant: root is always page 1
        write_page(0, std::move(fresh));
    }

    // 2. write the rollback journal (before any dirty page hits the file)
    if (file_backed_) {
        JournalData jd;
        jd.page_size = page_size_;
        jd.original_page_count = original_page_count_;
        jd.pages = txn_orig_;
        journal::write(path_, jd);
    }

    // 3. flush dirty pages + fsync
    flush_all();

    // 4. commit point: remove the journal
    if (file_backed_) journal::remove(path_);

    txn_active_ = false;
    txn_orig_.clear();
}

void Pager::rollback_txn() {
    if (!txn_active_)
        throw DbError::txn("cannot roll back: no transaction is active");
    if (stmt_active_) {
        stmt_images_.clear();
        stmt_active_ = false;
    }

    for (const auto& [id, img] : txn_orig_)
        store_->write(id, img.data(), page_size_);
    if (page_count_ > original_page_count_)
        store_->truncate_to(original_page_count_, page_size_);
    store_->sync();

    // a half-finished commit may have left a journal on disk
    if (file_backed_ && journal::exists(path_))
        journal::remove(path_);

    // drop all cached state; everything is re-read from the restored file
    cache_.clear();
    page_count_ = original_page_count_;
    txn_active_ = false;
    txn_orig_.clear();
}

// ---- open / create ----

std::unique_ptr<Pager> Pager::open_file(const std::string& path, uint32_t page_size) {
    auto pager = std::unique_ptr<Pager>(new Pager());
    pager->path_ = path;
    pager->file_backed_ = true;

    // Does the file exist and have content?
    bool fresh = true;
    {
        std::FILE* probe = std::fopen(path.c_str(), "rb");
        if (probe) {
            if (std::fseek(probe, 0, SEEK_END) == 0 && std::ftell(probe) > 0) fresh = false;
            std::fclose(probe);
        }
    }

    if (fresh) {
        check_page_size(page_size);
        pager->page_size_ = page_size;
        pager->store_ = std::make_unique<FileStore>(path, true);
        // page 0: header; page 1: empty catalog root (table leaf)
        pager->page_count_ = 2;
        auto h = hdr::make_image(page_size, 2, 0, 0, 0);
        pager->store_->write(0, h.data(), page_size);
        Page p1(page_size);
        init_page(p1, PageType::TableLeaf);
        pager->store_->write(1, p1.data(), page_size);
        pager->store_->sync();
        return pager;
    }

    // Existing database: recover from any hot journal first.
    if (journal::exists(path))
        journal::recover_if_present(path);

    pager->store_ = std::make_unique<FileStore>(path, false);
    // The page-size field lives in the first 12 bytes; a prefix read is
    // enough to learn the real page size before committing to it.
    auto p0 = pager->store_->read_prefix(4096);
    if (p0.size() < hdr::HEADER_USED)
        throw DbError::storage(str("file ", path, " is too small to be a database"));
    hdr::HeaderInfo h = hdr::parse(p0);
    pager->page_size_ = h.page_size;
    pager->page_count_ = h.page_count;
    if (pager->page_count_ < 2)
        throw DbError::storage(str("database header claims an impossible page count ",
                                   pager->page_count_));
    uint64_t want = uint64_t(pager->page_count_) * pager->page_size_;
    uint64_t have = pager->store_->size_bytes();
    if (have != want)
        throw DbError::storage(str("database file size (", have,
                                   " bytes) does not match its header (", want,
                                   " bytes): the file is truncated or corrupt"));
    return pager;
}

std::unique_ptr<Pager> Pager::open_memory(uint32_t page_size) {
    check_page_size(page_size);
    auto pager = std::unique_ptr<Pager>(new Pager());
    pager->path_ = ":memory:";
    pager->file_backed_ = false;
    pager->page_size_ = page_size;
    pager->store_ = std::make_unique<MemStore>();
    pager->page_count_ = 2;
    auto h = hdr::make_image(page_size, 2, 0, 0, 0);
    pager->store_->write(0, h.data(), page_size);
    Page p1(page_size);
    init_page(p1, PageType::TableLeaf);
    pager->store_->write(1, p1.data(), page_size);
    return pager;
}

} // namespace sc
