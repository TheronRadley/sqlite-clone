// test_page.cpp — page container and header/cell-area tests.
#include "test_util.hpp"

#include "page.hpp"

using namespace sc;

namespace {

Page fresh(PageType t) {
    Page p(512);
    init_page(p, t);
    return p;
}

} // namespace

TEST(page_init_leaf) {
    Page p = fresh(PageType::TableLeaf);
    CHECK(page_type(p) == PageType::TableLeaf);
    CHECK(page_type_is_leaf(page_type(p)));
    CHECK_EQ(cell_count(p), uint16_t(0));
    CHECK_EQ(content_start(p), uint16_t(512));
    CHECK_EQ(garbage_bytes(p), uint16_t(0));
    CHECK_EQ(header_size(p), size_t(8));
    validate_page_header(p);
}

TEST(page_init_interior) {
    Page p = fresh(PageType::IndexInterior);
    CHECK(!page_type_is_leaf(page_type(p)));
    CHECK(page_type_is_btree(page_type(p)));
    CHECK_EQ(header_size(p), size_t(12));
    CHECK_EQ(right_child(p), uint32_t(0));
    set_right_child(p, 77);
    CHECK_EQ(right_child(p), uint32_t(77));
    CHECK_EQ(content_start(p), uint16_t(512));   // interior header occupies 12
    validate_page_header(p);
}

TEST(page_type_bytes) {
    CHECK_EQ(page_type_name(PageType::TableLeaf), std::string("table leaf"));
    CHECK_EQ(page_type_name(PageType::FreelistTrunk), std::string("freelist trunk"));
    Page p(512);
    init_page(p, PageType::TableLeaf);
    p.b[0] = uint8_t(PageType::TableInterior);
    CHECK(page_type(p) == PageType::TableInterior);
    CHECK_EQ(header_size(p), size_t(12));   // header size follows the type byte
}

TEST(page_insert_remove_cells) {
    Page p = fresh(PageType::TableLeaf);
    uint8_t cell[4] = {1, 2, 3, 4};
    insert_cell(p, 0, cell, 4);
    CHECK_EQ(cell_count(p), uint16_t(1));
    CHECK_EQ(content_start(p), uint16_t(512 - 4));
    CHECK_EQ(cell_offset(p, 0), uint16_t(508));
    uint8_t cell2[2] = {9, 9};
    insert_cell(p, 0, cell2, 2);   // insert in front
    CHECK_EQ(cell_count(p), uint16_t(2));
    CHECK_EQ(cell_offset(p, 0), uint16_t(506));
    CHECK_EQ(cell_offset(p, 1), uint16_t(508));
    // free = content_start - (header + 2 pointers) - one reserved slot
    CHECK_EQ(free_contiguous(p), size_t(512 - 8 - 4 - 6 - 2));
    CHECK_EQ(p.b[506], uint8_t(9));
    CHECK_EQ(p.b[508], uint8_t(1));
    validate_page_header(p);

    // removing the LOWEST cell (the one at content_start) shrinks the
    // content area directly: no garbage
    remove_cell(p, 0, 2);
    CHECK_EQ(garbage_bytes(p), uint16_t(0));
    CHECK_EQ(content_start(p), uint16_t(508));
    CHECK_EQ(cell_count(p), uint16_t(1));
    CHECK_EQ(p.b[cell_offset(p, 0)], uint8_t(1));
    remove_cell(p, 0, 4);
    CHECK_EQ(cell_count(p), uint16_t(0));
    CHECK_EQ(content_start(p), uint16_t(512));
    CHECK_EQ(garbage_bytes(p), uint16_t(0));
}

TEST(page_garbage_accounting) {
    Page p = fresh(PageType::TableLeaf);
    for (int i = 0; i < 6; ++i) {
        uint8_t c[4] = {uint8_t(i), 0, 0, 0};
        insert_cell(p, i, c, 4);
    }
    CHECK_EQ(content_start(p), uint16_t(512 - 24));
    // removing a MIDDLE cell (not at content_start) creates garbage
    remove_cell(p, 1, 4);   // removes logical cell {1}
    CHECK_EQ(garbage_bytes(p), uint16_t(4));
    remove_cell(p, 2, 4);   // removes logical cell {3} (indices shifted)
    CHECK_EQ(garbage_bytes(p), uint16_t(8));
    CHECK_EQ(cell_count(p), uint16_t(4));
    CHECK_EQ(content_start(p), uint16_t(512 - 24));   // unchanged
    validate_page_header(p);

    // live cells are untouched and still readable in order: {0,2,4,5}
    const uint8_t want[4] = {0, 2, 4, 5};
    for (int i = 0; i < 4; ++i)
        CHECK_EQ(p.b[cell_offset(p, i)], want[i]);

    // new cells still land below content_start; garbage is not reclaimed
    // by insertion (only by rebuilding the page)
    uint8_t c[4] = {7, 7, 7, 7};
    insert_cell(p, 4, c, 4);
    CHECK_EQ(garbage_bytes(p), uint16_t(8));
    CHECK_EQ(cell_count(p), uint16_t(5));
    CHECK_EQ(p.b[cell_offset(p, 4)], uint8_t(7));
    validate_page_header(p);
}

TEST(page_fill_to_capacity) {
    Page p = fresh(PageType::TableLeaf);
    size_t inserted = 0;
    for (;;) {
        uint8_t c[8] = {};
        if (8 > free_contiguous(p)) break;
        insert_cell(p, int(inserted), c, 8);
        ++inserted;
    }
    CHECK(inserted > 40);   // (512 - 8) / (8 + 2) = 50 cells
    CHECK_EQ(garbage_bytes(p), uint16_t(0));
    // one more must fail loudly
    uint8_t c[8] = {};
    CHECK_DB_ERROR(insert_cell(p, int(inserted), c, 8), int(Err::BTree),
                   "does not fit");
    validate_page_header(p);
}

TEST(page_header_validation_rejects_garbage) {
    {
        Page p(512);
        init_page(p, PageType::TableLeaf);
        p.b[0] = 99;   // impossible type
        CHECK_DB_ERROR(validate_page_header(p), int(Err::BTree),
                       "invalid page type byte 99");
    }
    {
        Page p(512);
        init_page(p, PageType::TableLeaf);
        p.b[1] = 7;    // flags must be 0
        CHECK_DB_ERROR(validate_page_header(p), int(Err::BTree), "flag byte");
    }
    {
        Page p(512);
        init_page(p, PageType::TableLeaf);
        set_content_start(p, 600);   // beyond the page
        CHECK_DB_ERROR(validate_page_header(p), int(Err::BTree),
                       "beyond page size");
    }
    {
        Page p(512);
        init_page(p, PageType::TableLeaf);
        set_cell_count(p, 300);      // pointer array would overlap content start
        CHECK_DB_ERROR(validate_page_header(p), int(Err::BTree),
                       "overlaps content start");
    }
    {
        Page p(512);
        init_page(p, PageType::TableLeaf);
        set_garbage(p, 5000);        // more garbage than the page holds
        CHECK_DB_ERROR(validate_page_header(p), int(Err::BTree),
                       "garbage byte count");
    }
}

TEST(page_cell_offset_bounds) {
    Page p = fresh(PageType::TableLeaf);
    CHECK_DB_ERROR((void)cell_offset(p, 0), int(Err::BTree), "out of range");
    uint8_t c[2] = {};
    insert_cell(p, 0, c, 2);
    CHECK_DB_ERROR((void)cell_offset(p, 1), int(Err::BTree), "out of range");
    CHECK_DB_ERROR((void)cell_offset(p, -1), int(Err::BTree), "out of range");
    CHECK_DB_ERROR(set_cell_offset(p, 5, 4), int(Err::BTree), "out of range");
}
