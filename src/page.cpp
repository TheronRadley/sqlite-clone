#include "page.hpp"

#include "util.hpp"

namespace sc {

const char* page_type_name(PageType t) {
    switch (t) {
        case PageType::TableLeaf: return "table leaf";
        case PageType::TableInterior: return "table interior";
        case PageType::IndexLeaf: return "index leaf";
        case PageType::IndexInterior: return "index interior";
        case PageType::FreelistTrunk: return "freelist trunk";
        case PageType::FreelistLeaf: return "freelist leaf";
    }
    return "?";
}

bool page_type_is_leaf(PageType t) {
    return t == PageType::TableLeaf || t == PageType::IndexLeaf;
}

bool page_type_is_btree(PageType t) {
    return t == PageType::TableLeaf || t == PageType::TableInterior ||
           t == PageType::IndexLeaf || t == PageType::IndexInterior;
}

PageType page_type(const Page& p) {
    if (p.size() == 0) throw DbError::btree("page has zero size");
    uint8_t t = p.b[0];
    if (t < 1 || t > 6)
        throw DbError::btree(str("invalid page type byte ", int(t)));
    return PageType(t);
}

size_t header_size(const Page& p) {
    return page_type_is_leaf(page_type(p)) ? LEAF_HEADER_SIZE : INTERIOR_HEADER_SIZE;
}

uint16_t cell_count(const Page& p) { return rd_be16(p.b.data() + 2); }
void set_cell_count(Page& p, uint16_t n) { wr_be16(p.b.data() + 2, n); }

uint16_t content_start(const Page& p) { return rd_be16(p.b.data() + 4); }
void set_content_start(Page& p, uint16_t v) { wr_be16(p.b.data() + 4, v); }

uint16_t garbage_bytes(const Page& p) { return rd_be16(p.b.data() + 6); }
void set_garbage(Page& p, uint16_t v) { wr_be16(p.b.data() + 6, v); }

uint32_t right_child(const Page& p) { return rd_be32(p.b.data() + 8); }
void set_right_child(Page& p, uint32_t v) { wr_be32(p.b.data() + 8, v); }

uint16_t cell_offset(const Page& p, int i) {
    if (i < 0 || size_t(i) >= cell_count(p))
        throw DbError::btree(str("cell index ", i, " out of range (count ",
                                 cell_count(p), ")"));
    return rd_be16(p.b.data() + header_size(p) + CELL_PTR_SIZE * size_t(i));
}

void set_cell_offset(Page& p, int i, uint16_t off) {
    if (i < 0 || size_t(i) >= cell_count(p))
        throw DbError::btree(str("cell index ", i, " out of range"));
    wr_be16(p.b.data() + header_size(p) + CELL_PTR_SIZE * size_t(i), off);
}

void validate_page_header(const Page& p) {
    PageType t = page_type(p);       // throws on bad type byte
    if (p.b[1] != 0)
        throw DbError::btree(str("page flag byte is ", int(p.b[1]), " (must be 0)"));
    uint16_t count = cell_count(p);
    uint16_t cstart = content_start(p);
    uint16_t garbage = garbage_bytes(p);
    size_t hdr = page_type_is_leaf(t) ? LEAF_HEADER_SIZE : INTERIOR_HEADER_SIZE;
    if (size_t(hdr) + CELL_PTR_SIZE * count > cstart)
        throw DbError::btree(str("cell pointer array (", hdr + CELL_PTR_SIZE * count,
                                 " bytes) overlaps content start (", cstart, ")"));
    if (cstart > p.size())
        throw DbError::btree(str("content start ", cstart, " beyond page size ", p.size()));
    if (size_t(garbage) > p.size() - cstart)
        throw DbError::btree(str("garbage byte count ", garbage,
                                 " larger than content area"));
    if (!page_type_is_leaf(t) && p.size() < INTERIOR_HEADER_SIZE)
        throw DbError::btree("interior page smaller than its header");
}

size_t free_contiguous(const Page& p) {
    // content_start - (header + 2*count) - 2 (slot for the new pointer)
    size_t avail = size_t(content_start(p)) - ptr_array_end(p);
    return avail > CELL_PTR_SIZE ? avail - CELL_PTR_SIZE : 0;
}

void insert_cell(Page& p, int i, const uint8_t* cell, size_t len) {
    uint16_t count = cell_count(p);
    if (i < 0 || size_t(i) > count)
        throw DbError::btree(str("insert_cell: bad index ", i));
    if (len > free_contiguous(p))
        throw DbError::btree(str("insert_cell: cell of ", len,
                                 " bytes does not fit (free ", free_contiguous(p), ")"));
    if (count + 1 > 0xffff)
        throw DbError::btree("cell count overflow");

    size_t hdr = header_size(p);
    // shift pointer entries right of i by one slot
    for (int k = count; k > i; --k) {
        uint16_t v = rd_be16(p.b.data() + hdr + CELL_PTR_SIZE * size_t(k - 1));
        wr_be16(p.b.data() + hdr + CELL_PTR_SIZE * size_t(k), v);
    }
    uint16_t cstart = uint16_t(content_start(p) - len);
    set_content_start(p, cstart);
    std::copy(cell, cell + len, p.b.begin() + cstart);
    wr_be16(p.b.data() + hdr + CELL_PTR_SIZE * size_t(i), cstart);
    set_cell_count(p, uint16_t(count + 1));
}

void remove_cell(Page& p, int i, size_t len) {
    uint16_t count = cell_count(p);
    if (i < 0 || size_t(i) >= count)
        throw DbError::btree(str("remove_cell: bad index ", i));
    size_t hdr = header_size(p);
    uint16_t off = cell_offset(p, i);
    // shift pointer entries left of the removed one... entries after i shift left
    for (int k = i; k + 1 < count; ++k) {
        uint16_t v = rd_be16(p.b.data() + hdr + CELL_PTR_SIZE * size_t(k + 1));
        wr_be16(p.b.data() + hdr + CELL_PTR_SIZE * size_t(k), v);
    }
    set_cell_count(p, uint16_t(count - 1));
    if (off == content_start(p)) {
        // lowest cell: shrink the content area directly, no garbage
        set_content_start(p, uint16_t(off + len));
    } else {
        set_garbage(p, uint16_t(garbage_bytes(p) + len));
    }
}

void init_page(Page& p, PageType type) {
    std::fill(p.b.begin(), p.b.end(), 0);
    p.b[0] = uint8_t(type);
    set_cell_count(p, 0);
    set_content_start(p, uint16_t(p.size()));
    set_garbage(p, 0);
    // right_child stays 0
}

} // namespace sc
