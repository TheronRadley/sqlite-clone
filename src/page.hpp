// page.hpp — the fixed-size page container and its header layout.
//
// A page is a flat byte array; all structure inside it is accessed through
// the helpers below with named constants. Nothing else in the engine reads
// page bytes directly.
//
// B-Tree page layout (see STORAGE_FORMAT.md):
//
//   offset  size  meaning
//   0       1     page type (PageType below)
//   1       1     flags (reserved, must be 0)
//   2       2     cell count (u16 BE)
//   4       2     cell content area start (u16 BE; == page_size when empty)
//   6       2     garbage bytes inside the content area (u16 BE)
//   8       4     right-most child pointer (u32 BE; interior pages only)
//   8/12    ...   cell pointer array: u16 BE offsets, one per cell, ordered
//   ...     ...   cell content area, growing DOWN from the end of the page
//
// Freelist trunk pages reuse the same container with their own type and
// body format (see STORAGE_FORMAT.md); the pager does not interpret them.
#pragma once

#include <cstdint>
#include <vector>

#include "error.hpp"

namespace sc {

enum class PageType : uint8_t {
    TableLeaf = 1,
    TableInterior = 2,
    IndexLeaf = 3,
    IndexInterior = 4,
    FreelistTrunk = 5,
    FreelistLeaf = 6,   // marker for pages listed in a trunk; content is stale
};

const char* page_type_name(PageType t);
bool page_type_is_leaf(PageType t);
bool page_type_is_btree(PageType t);

inline constexpr size_t LEAF_HEADER_SIZE = 8;
inline constexpr size_t INTERIOR_HEADER_SIZE = 12;
inline constexpr size_t CELL_PTR_SIZE = 2;

struct Page {
    std::vector<uint8_t> b;

    Page() = default;
    explicit Page(size_t page_size) : b(page_size, 0) {}
    explicit Page(std::vector<uint8_t> bytes) : b(std::move(bytes)) {}

    size_t size() const { return b.size(); }
    uint8_t* data() { return b.data(); }
    const uint8_t* data() const { return b.data(); }
    uint8_t& at(size_t i) { return b[i]; }
    uint8_t at(size_t i) const { return b[i]; }
};

// ---- header field access; all throw BTreeError when the page type or a
// field value is impossible (disk data is never trusted) ----

PageType page_type(const Page& p);
size_t header_size(const Page& p);
uint16_t cell_count(const Page& p);
void set_cell_count(Page& p, uint16_t n);
uint16_t content_start(const Page& p);
void set_content_start(Page& p, uint16_t v);
uint16_t garbage_bytes(const Page& p);
void set_garbage(Page& p, uint16_t v);
uint32_t right_child(const Page& p);
void set_right_child(Page& p, uint32_t v);

uint16_t cell_offset(const Page& p, int i);
void set_cell_offset(Page& p, int i, uint16_t off);
inline size_t ptr_array_end(const Page& p) {
    return header_size(p) + CELL_PTR_SIZE * cell_count(p);
}

// ---- structural checks ----

// Throws BTreeError if header fields are impossible (bad type, content
// start overlapping the pointer array, garbage larger than the content
// area, ...). Called before any page is used.
void validate_page_header(const Page& p);

// ---- cell area operations (no knowledge of cell contents) ----

// Contiguous bytes available for a new cell of length `len` (reserving one
// more pointer-array slot). Does NOT account garbage reclamation.
size_t free_contiguous(const Page& p);

// Insert raw cell bytes at logical index `i`. Throws BTreeError if the
// cell does not fit (caller must reclaim garbage or split first).
void insert_cell(Page& p, int i, const uint8_t* cell, size_t len);

// Remove the cell at logical index `i` whose length is `len` (caller knows
// the length from parsing). Updates garbage accounting. Garbage is later
// reclaimed by rebuilding the page from its live cells (btree.cpp
// rebuild_page); there is no in-place compaction.
void remove_cell(Page& p, int i, size_t len);

// Initialize a fresh page of the given type.
void init_page(Page& p, PageType type);

} // namespace sc
