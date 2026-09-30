// btree.cpp — B+-Tree implementation (see btree.hpp and B_TREE.md).
//
// Implementation notes:
//   * Pages are read as copies through the pager, modified, and written
//     back. Splits and merges REBUILD pages from parsed cell lists, which
//     keeps the pointer array compact and garbage-free.
//   * All cell parsing is bounds-checked; malformed disk data raises
//     BTreeError, never undefined behavior.
//   * Splits are normally a byte-balanced 2-way cut chosen by scanning all
//     valid boundaries. Tightly packed pages can defeat every 2-way cut
//     (e.g. three near-half-page cells), so the cascade then falls back to
//     a greedy multi-way split; a feasible plan always exists because a
//     single cell always fits (table leaf cells are capped by the overflow
//     threshold, index cells by index_key_max).
#include "btree.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <sstream>

#include "util.hpp"
#include "varint.hpp"

namespace sc {

namespace btree_impl {

// ---------- shared constants ----------

// Payload placement (see STORAGE_FORMAT.md). Constants leave slack for the
// page header, the cell pointer array, and the overflow page pointer.
inline size_t local_max(uint32_t ps) { return ps - 64; }
inline size_t overflow_local_size(uint32_t ps) { return ps - 128; }
// Index keys must leave room for two cells plus headers in any node so
// that an interior split always has a valid split point (see B_TREE.md).
inline size_t index_key_max(uint32_t ps) { return ps / 2 - 48; }

constexpr uint64_t MAX_PAYLOAD = 1ull << 30;  // 1 GiB sanity bound

// Overflow page: [u32 next page][payload bytes...]
constexpr size_t OV_NEXT_OFF = 0;
constexpr size_t OV_DATA_OFF = 4;

template <class K>
PageType leaf_type_for() {
    return K::node_kind == NodeKind::Table ? PageType::TableLeaf : PageType::IndexLeaf;
}

template <class K>
PageType interior_type_for() {
    return K::node_kind == NodeKind::Table ? PageType::TableInterior
                                           : PageType::IndexInterior;
}

// A parsed cell together with its raw in-page bytes.
template <class K>
struct ParsedCell {
    CellData<K> cd;
    std::vector<uint8_t> bytes;
};

// ---------- overflow chains ----------

inline uint32_t write_overflow_chain(Pager& pager, const uint8_t* rest, size_t rest_len) {
    uint32_t ps = pager.page_size();
    size_t cap = ps - OV_DATA_OFF;
    size_t npages = (rest_len + cap - 1) / cap;
    if (npages == 0) npages = 1;
    if (npages > (1u << 22))
        throw DbError::constraint("payload too large: overflow chain exceeds page budget");

    std::vector<uint32_t> ids;
    ids.reserve(npages);
    for (size_t i = 0; i < npages; ++i) ids.push_back(pager.allocate_page());

    size_t off = 0;
    for (size_t i = 0; i < npages; ++i) {
        Page p(pager.page_size());
        wr_be32(p.data() + OV_NEXT_OFF, i + 1 < npages ? ids[i + 1] : 0);
        size_t chunk = std::min(cap, rest_len - off);
        std::memcpy(p.data() + OV_DATA_OFF, rest + off, chunk);
        off += chunk;
        pager.write_page(ids[i], std::move(p.b));
    }
    return ids[0];
}

inline std::vector<uint8_t> read_full_payload(Pager& pager,
                                              const std::vector<uint8_t>& local,
                                              uint32_t first_page, uint32_t payload_len) {
    uint32_t ps = pager.page_size();
    if (payload_len > uint64_t(pager.page_count()) * ps)
        throw DbError::btree(str("payload length ", payload_len,
                                 " exceeds the size of the whole database"));
    std::vector<uint8_t> out;
    out.reserve(payload_len);
    out.insert(out.end(), local.begin(), local.end());
    size_t need = payload_len - local.size();
    uint32_t page = first_page;
    while (need > 0) {
        if (page == 0 || page >= pager.page_count())
            throw DbError::btree(str("overflow chain references invalid page ", page));
        Page p(pager.get_page(page));
        uint32_t next = rd_be32(p.data() + OV_NEXT_OFF);
        size_t chunk = std::min(need, ps - OV_DATA_OFF);
        out.insert(out.end(), p.data() + OV_DATA_OFF, p.data() + OV_DATA_OFF + chunk);
        need -= chunk;
        if (need > 0 && next == 0)
            throw DbError::btree("overflow chain is truncated (needs more pages)");
        if (need == 0 && next != 0)
            throw DbError::btree("overflow chain is longer than the payload (possible cycle)");
        page = next;
    }
    if (out.size() != payload_len)
        throw DbError::btree("overflow chain produced the wrong number of bytes");
    return out;
}

inline void free_overflow_chain(Pager& pager, uint32_t first_page) {
    std::vector<uint32_t> chain;
    uint32_t page = first_page;
    while (page != 0) {
        if (page >= pager.page_count())
            throw DbError::btree(str("overflow chain references invalid page ", page));
        if (chain.size() > pager.page_count())
            throw DbError::btree("overflow chain contains a cycle");
        Page p(pager.get_page(page));
        chain.push_back(page);
        page = rd_be32(p.data() + OV_NEXT_OFF);
    }
    for (uint32_t id : chain) pager.free_page(id);
}

inline void validate_overflow_chain(Pager& pager, uint32_t first_page, uint32_t payload_len,
                                    size_t local_size,
                                    std::vector<uint32_t>* chain_pages = nullptr) {
    size_t cap = pager.page_size() - OV_DATA_OFF;
    size_t need = payload_len > local_size ? payload_len - local_size : 0;
    uint32_t page = first_page;
    std::set<uint32_t> seen;
    while (need > 0) {
        if (page == 0 || page >= pager.page_count())
            throw DbError::btree(str("overflow chain references invalid page ", page));
        if (!seen.insert(page).second)
            throw DbError::btree(str("overflow chain visits page ", page, " twice (cycle)"));
        if (chain_pages) chain_pages->push_back(page);
        Page p(pager.get_page(page));
        uint32_t next = rd_be32(p.data() + OV_NEXT_OFF);
        need = need > cap ? need - cap : 0;
        if (need == 0 && next != 0)
            throw DbError::btree(str("overflow chain of page ", first_page,
                                     " continues past the payload end"));
        page = next;
    }
}

// ---------- node / cell primitives ----------

template <class K>
Page load_node(Pager& pager, uint32_t id) {
    Page p(pager.get_page(id));
    validate_page_header(p);
    PageType t = page_type(p);
    if (t != leaf_type_for<K>() && t != interior_type_for<K>())
        throw DbError::btree(str("page ", id, " has type ", page_type_name(t),
                                 " but this tree requires ", page_type_name(leaf_type_for<K>()),
                                 " or ", page_type_name(interior_type_for<K>())));
    return p;
}

template <class K>
CellData<K> parse_cell(const Page& p, PageType t, int i) {
    uint16_t off = cell_offset(p, i);
    size_t avail = p.size() - off;
    const uint8_t* d = p.data() + off;
    CellData<K> cd;

    if constexpr (K::node_kind == NodeKind::Table) {
        if (t == PageType::TableLeaf) {
            // varint(payload_len) key8 [payload | prefix + ov_page]
            size_t used = 0;
            uint64_t plen = get_varint(d, avail, used);
            if (plen > MAX_PAYLOAD)
                throw DbError::btree(
                    str("cell declares an impossible payload length ", plen));
            if (avail < used + 8)
                throw DbError::btree("table leaf cell is truncated (key)");
            cd.key = RowIdKey::decode(d + used, avail - used);
            size_t after_key = used + 8;
            if (plen <= local_max(uint32_t(p.size()))) {
                if (avail < after_key + plen)
                    throw DbError::btree("table leaf cell payload runs past the page end");
                cd.payload_len = uint32_t(plen);
                cd.local.assign(d + after_key, d + after_key + plen);
                cd.size = after_key + plen;
            } else {
                size_t loc = overflow_local_size(uint32_t(p.size()));
                if (avail < after_key + loc + 4)
                    throw DbError::btree("table leaf cell with overflow is truncated");
                cd.payload_len = uint32_t(plen);
                cd.local.assign(d + after_key, d + after_key + loc);
                cd.overflow = rd_be32(d + after_key + loc);
                cd.has_overflow = true;
                cd.size = after_key + loc + 4;
            }
        } else if (t == PageType::TableInterior) {
            if (avail < 12) throw DbError::btree("table interior cell is truncated");
            cd.child = rd_be32(d);
            cd.key = RowIdKey::decode(d + 4, avail - 4);
            cd.size = 12;
        } else {
            throw DbError::btree("internal error: page type / key kind mismatch");
        }
    } else {
        if (t == PageType::IndexLeaf) {
            size_t used = 0;
            cd.key = IndexKey::decode(d, avail, used);
            cd.size = used;
        } else if (t == PageType::IndexInterior) {
            if (avail < 5) throw DbError::btree("index interior cell is truncated");
            cd.child = rd_be32(d);
            size_t used = 0;
            cd.key = IndexKey::decode(d + 4, avail - 4, used);
            cd.size = 4 + used;
        } else {
            throw DbError::btree("internal error: page type / key kind mismatch");
        }
    }
    if (cd.size > avail)
        throw DbError::btree(str("cell at offset ", off, " runs past the page end"));
    return cd;
}

template <class K>
std::vector<ParsedCell<K>> parse_all_cells(const Page& p) {
    PageType t = page_type(p);
    std::vector<ParsedCell<K>> out;
    out.reserve(cell_count(p));
    for (int i = 0; i < int(cell_count(p)); ++i) {
        ParsedCell<K> pc;
        pc.cd = parse_cell<K>(p, t, i);
        uint16_t off = cell_offset(p, i);
        pc.bytes.assign(p.data() + off, p.data() + off + pc.cd.size);
        out.push_back(std::move(pc));
    }
    return out;
}

template <class K>
ParsedCell<K> make_interior_parsed(uint32_t child, const K& key) {
    ParsedCell<K> pc;
    pc.cd.key = key;
    pc.cd.child = child;
    std::vector<uint8_t> out;
    uint8_t b4[4];
    wr_be32(b4, child);
    out.insert(out.end(), b4, b4 + 4);
    key.encode(out);
    pc.bytes = std::move(out);
    pc.cd.size = pc.bytes.size();
    return pc;
}

template <class K>
std::vector<uint8_t> build_leaf_cell(Pager& pager, const K& key,
                                     const std::vector<uint8_t>* payload) {
    std::vector<uint8_t> out;
    if constexpr (K::node_kind == NodeKind::Table) {
        uint32_t ps = pager.page_size();
        uint64_t plen = payload->size();
        if (plen > MAX_PAYLOAD)
            throw DbError::constraint(str("row of ", plen,
                                          " bytes exceeds the 1 GiB payload limit"));
        put_varint(out, plen);
        key.encode(out);
        if (plen <= local_max(ps)) {
            out.insert(out.end(), payload->begin(), payload->end());
        } else {
            size_t loc = overflow_local_size(ps);
            out.insert(out.end(), payload->begin(), payload->begin() + loc);
            uint32_t first = write_overflow_chain(pager, payload->data() + loc, plen - loc);
            uint8_t b4[4];
            wr_be32(b4, first);
            out.insert(out.end(), b4, b4 + 4);
        }
    } else {
        if (payload != nullptr)
            throw DbError::btree("internal error: index trees have no payload");
        key.encode(out);
        if (out.size() > index_key_max(pager.page_size()))
            throw DbError::constraint(
                str("indexed value too long: key of ", out.size(), " bytes exceeds the ",
                    index_key_max(pager.page_size()), "-byte limit (page size ",
                    pager.page_size(), ")"));
    }
    return out;
}

// Does a list of cells fit as a standalone page? (Conservative: uses the
// larger interior header size.)
template <class K>
bool list_fits(uint32_t page_size, const std::vector<ParsedCell<K>>& cells) {
    size_t sum = 0;
    for (const auto& c : cells) sum += c.bytes.size();
    return INTERIOR_HEADER_SIZE + 2 * cells.size() + sum <= page_size;
}

template <class K>
void rebuild_page(Page& p, PageType t, const std::vector<ParsedCell<K>>& cells,
                  uint32_t right = 0) {
    init_page(p, t);
    if (t == PageType::TableInterior || t == PageType::IndexInterior)
        set_right_child(p, right);
    for (size_t i = 0; i < cells.size(); ++i)
        insert_cell(p, int(i), cells[i].bytes.data(), cells[i].bytes.size());
}

// first i with key <= cells[i].key (child routing); count means rightmost
template <class K>
int interior_slot(const Page& p, const K& key) {
    int lo = 0, hi = int(cell_count(p));
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        CellData<K> cd = parse_cell<K>(p, page_type(p), mid);
        if (key.compare(cd.key) <= 0) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

// first i with cells[i].key >= key; *found = exact match
template <class K>
int leaf_lower_bound(const Page& p, const K& key, bool* found) {
    int lo = 0, hi = int(cell_count(p));
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        CellData<K> cd = parse_cell<K>(p, page_type(p), mid);
        if (cd.key.compare(key) >= 0) hi = mid;
        else lo = mid + 1;
    }
    *found = false;
    if (lo < int(cell_count(p))) {
        CellData<K> cd = parse_cell<K>(p, page_type(p), lo);
        *found = cd.key.compare(key) == 0;
    }
    return lo;
}

// Max key of a node's subtree (rightmost spine).
template <class K>
K max_of_node(Pager& pager, const Page& p) {
    if (page_type_is_leaf(page_type(p))) {
        if (cell_count(p) == 0)
            throw DbError::btree("internal error: max of an empty node");
        return parse_cell<K>(p, page_type(p), int(cell_count(p)) - 1).key;
    }
    uint32_t cur = right_child(p);
    for (;;) {
        Page n = load_node<K>(pager, cur);
        if (page_type_is_leaf(page_type(n))) return max_of_node<K>(pager, n);
        cur = right_child(n);
    }
}

// ---------- split point selection ----------
//
// A split partitions the ordered cell list into consecutive runs, one run
// per output page:
//   * leaf runs keep every cell they contain
//   * non-final interior runs keep all but their LAST cell, which is
//     consumed: its child becomes that page's right pointer and its key
//     is promoted to the parent as the separator
//   * the final run keeps everything and inherits the node's old right
//     pointer
// The separator above a non-final run is always that run's last cell key
// (for leaves: the last key kept; for interiors: the consumed separator) —
// the same expression either way.
//
// choose_split produces the normal balanced 2-run split by scanning every
// feasible boundary. Pathological size distributions (e.g. three
// near-half-page cells) can make every 2-way partition infeasible, so the
// caller falls back to greedy_runs, which packs as many runs as needed.
// A feasible plan therefore always exists: single cells always fit (table
// leaf cells are capped by the overflow threshold, index cells by
// index_key_max), so the greedy walk can always make progress.

template <class K>
std::optional<size_t> choose_split(const std::vector<ParsedCell<K>>& cells, bool leaf,
                                   uint32_t ps) {
    size_t n = cells.size();
    size_t jmin = leaf ? 1 : 2;
    if (n < jmin + 1)
        throw DbError::btree("internal error: node too small to split");

    std::vector<size_t> prefix(n + 1, 0);
    for (size_t i = 0; i < n; ++i) prefix[i + 1] = prefix[i] + cells[i].bytes.size();
    size_t total = prefix[n];

    auto fits = [&](size_t count, size_t bytes) {
        return INTERIOR_HEADER_SIZE + 2 * count + bytes <= ps;
    };

    long best_score = -1;
    size_t best_j = 0;
    for (size_t j = jmin; j <= n - 1; ++j) {
        size_t left_count = leaf ? j : j - 1;
        size_t left_bytes = prefix[leaf ? j : j - 1];
        size_t right_count = n - j;
        size_t right_bytes = total - prefix[j];
        if (!fits(left_count, left_bytes) || !fits(right_count, right_bytes)) continue;
        // score: how close the left side is to half the bytes
        long diff = long(left_bytes > total / 2 ? left_bytes - total / 2
                                                : total / 2 - left_bytes);
        if (best_score < 0 || diff < best_score) {
            best_score = diff;
            best_j = j;
        }
    }
    if (best_score < 0) return std::nullopt;   // caller uses greedy_runs
    return best_j;
}

// Greedy fallback: accumulate cells into a run until it reaches ~half the
// usable space or the next cell would not fit, then cut. Non-final runs
// hold at least `minkeep` cells (1 for leaves; 2 for interiors so that one
// cell survives consumption). Every run fits by construction; the walk can
// never get stuck because minkeep cells always fit (see above).
template <class K>
std::vector<std::pair<size_t, size_t>> greedy_runs(
    const std::vector<ParsedCell<K>>& cells, bool leaf, uint32_t ps) {
    size_t n = cells.size();
    size_t cap = ps - INTERIOR_HEADER_SIZE;   // budget for 2*count + bytes
    size_t target = cap / 2;
    size_t minkeep = leaf ? 1 : 2;
    std::vector<std::pair<size_t, size_t>> runs;
    size_t start = 0, count = 0, bytes = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t cb = cells[i].bytes.size();
        bool next_fits = 2 * (count + 1) + bytes + cb <= cap;
        if (count >= minkeep && (!next_fits || bytes >= target)) {
            runs.emplace_back(start, i);   // run [start, i); cell i starts the next
            start = i;
            count = 0;
            bytes = 0;
            next_fits = 2 * 1 + cb <= cap;
        }
        if (!next_fits)
            throw DbError::btree("internal error: greedy split stuck "
                                 "(oversized cell)");
        ++count;
        bytes += cb;
    }
    runs.emplace_back(start, n);
    return runs;
}

// ---------- the split cascade (insertion) ----------

// Rebuild node `id` with `cells` (+ `right` for interior nodes), splitting
// upward until everything fits. `path` holds the ancestors of `id`.
template <class K>
void insert_cascade(std::vector<std::pair<uint32_t, int>>& path, uint32_t id,
                    PageType type, std::vector<ParsedCell<K>> cells, uint32_t right,
                    Pager& pager) {
    for (;;) {
        if (list_fits(pager.page_size(), cells)) {
            Page p(pager.page_size());
            rebuild_page(p, type, cells, right);
            pager.write_page(id, std::move(p.b));
            return;
        }

        if (path.empty()) {
            // Root split: re-initialize the root as a minimal interior node
            // whose right-most child is a fresh page, then let the loop
            // split the (still overflowing) cell list through the regular
            // path below — the child inherits type/cells/right untouched.
            // The root page id, recorded in the catalog, never changes.
            uint32_t child = pager.allocate_page();
            Page rp(pager.page_size());
            init_page(rp, interior_type_for<K>());
            set_right_child(rp, child);
            pager.write_page(id, std::move(rp.b));
            path.emplace_back(id, 0);   // child is the root's right-most child
            id = child;
            continue;
        }

        auto [pid, slot] = path.back();
        path.pop_back();
        Page parent = load_node<K>(pager, pid);
        PageType ptype = page_type(parent);

        // ---- split the node into consecutive runs ----
        bool leaf = (type == leaf_type_for<K>());
        size_t n = cells.size();
        std::vector<std::pair<size_t, size_t>> runs;
        if (auto j = choose_split(cells, leaf, pager.page_size())) {
            runs.assign(1, std::make_pair(size_t(0), *j));
            runs.emplace_back(*j, n);
        } else {
            runs = greedy_runs(cells, leaf, pager.page_size());
        }

        // materialize the pages: run 0 reuses `id`, the rest are fresh
        std::vector<uint32_t> page_ids{id};
        for (size_t t = 1; t < runs.size(); ++t)
            page_ids.push_back(pager.allocate_page());
        for (size_t t = 0; t < runs.size(); ++t) {
            auto [cs, ce] = runs[t];
            bool final_run = t + 1 == runs.size();
            Page p(pager.page_size());
            if (final_run) {
                rebuild_page(p, type,
                             std::vector<ParsedCell<K>>(cells.begin() + cs,
                                                       cells.begin() + ce),
                             right);
            } else if (leaf) {
                rebuild_page(p, type,
                             std::vector<ParsedCell<K>>(cells.begin() + cs,
                                                       cells.begin() + ce),
                             0);
            } else {
                // non-final interior run: the last cell is consumed
                rebuild_page(p, type,
                             std::vector<ParsedCell<K>>(cells.begin() + cs,
                                                       cells.begin() + (ce - 1)),
                             cells[ce - 1].cd.child);
            }
            pager.write_page(page_ids[t], std::move(p.b));
        }

        // ---- update the parent ----
        // The reference to `id` becomes one reference per run: runs
        // 0..k-2 keyed by their promoted separators, the final run keyed
        // by the old subtree maximum (or the right-most pointer).
        std::vector<ParsedCell<K>> pcells = parse_all_cells<K>(parent);
        uint32_t parent_right = right_child(parent);
        if (slot < int(pcells.size())) {
            K old_key = pcells[size_t(slot)].cd.key;
            std::vector<ParsedCell<K>> repl;
            for (size_t t = 0; t + 1 < runs.size(); ++t)
                repl.push_back(make_interior_parsed<K>(
                    page_ids[t], cells[runs[t].second - 1].cd.key));
            repl.push_back(make_interior_parsed<K>(page_ids.back(), old_key));
            pcells.erase(pcells.begin() + slot);
            pcells.insert(pcells.begin() + slot, repl.begin(), repl.end());
        } else {
            for (size_t t = 0; t + 1 < runs.size(); ++t)
                pcells.push_back(make_interior_parsed<K>(
                    page_ids[t], cells[runs[t].second - 1].cd.key));
            parent_right = page_ids.back();
        }

        id = pid;
        type = ptype;
        cells = std::move(pcells);
        right = parent_right;
    }
}

// ---------- borrow / merge (deletion) ----------

template <class K>
void borrow_from_left(Pager& pager, uint32_t parent_id, int slot, const Page& node,
                      const Page& lp) {
    Page parent = load_node<K>(pager, parent_id);
    std::vector<ParsedCell<K>> pcells = parse_all_cells<K>(parent);
    uint32_t parent_right = right_child(parent);
    auto child_at = [&](int s) -> uint32_t {
        return s < int(pcells.size()) ? pcells[size_t(s)].cd.child : parent_right;
    };
    uint32_t lid = child_at(slot - 1);
    uint32_t nid = child_at(slot);

    std::vector<ParsedCell<K>> lcells = parse_all_cells<K>(lp);
    std::vector<ParsedCell<K>> ncells = parse_all_cells<K>(node);

    if (page_type_is_leaf(page_type(node))) {
        // move the left sibling's last cell to the front of the node
        ncells.insert(ncells.begin(), lcells.back());
        lcells.pop_back();
        pcells[size_t(slot - 1)] =
            make_interior_parsed<K>(lid, lcells.back().cd.key);
        Page newl(pager.page_size());
        rebuild_page(newl, page_type(lp), lcells, 0);
        pager.write_page(lid, std::move(newl.b));
        Page newn(pager.page_size());
        rebuild_page(newn, page_type(node), ncells, 0);
        pager.write_page(nid, std::move(newn.b));
    } else {
        // node gains (left.right, parent separator); the left sibling's
        // last cell dissolves: child -> right pointer, key -> promoted
        ParsedCell<K> last = lcells.back();
        lcells.pop_back();
        ncells.insert(ncells.begin(),
                      make_interior_parsed<K>(right_child(lp),
                                              pcells[size_t(slot - 1)].cd.key));
        pcells[size_t(slot - 1)] = make_interior_parsed<K>(lid, last.cd.key);
        Page newl(pager.page_size());
        rebuild_page(newl, page_type(lp), lcells, last.cd.child);
        pager.write_page(lid, std::move(newl.b));
        Page newn(pager.page_size());
        rebuild_page(newn, page_type(node), ncells, right_child(node));
        pager.write_page(nid, std::move(newn.b));
    }
    Page pp(pager.page_size());
    rebuild_page(pp, page_type(parent), pcells, parent_right);
    pager.write_page(parent_id, std::move(pp.b));
}

template <class K>
void borrow_from_right(Pager& pager, uint32_t parent_id, int slot, const Page& node,
                       const Page& rp) {
    Page parent = load_node<K>(pager, parent_id);
    std::vector<ParsedCell<K>> pcells = parse_all_cells<K>(parent);
    uint32_t parent_right = right_child(parent);
    auto child_at = [&](int s) -> uint32_t {
        return s < int(pcells.size()) ? pcells[size_t(s)].cd.child : parent_right;
    };
    uint32_t rid = child_at(slot + 1);
    uint32_t nid = child_at(slot);

    std::vector<ParsedCell<K>> rcells = parse_all_cells<K>(rp);
    std::vector<ParsedCell<K>> ncells = parse_all_cells<K>(node);

    if (page_type_is_leaf(page_type(node))) {
        ncells.push_back(rcells.front());
        rcells.erase(rcells.begin());
        pcells[size_t(slot)] = make_interior_parsed<K>(nid, ncells.back().cd.key);
        Page newr(pager.page_size());
        rebuild_page(newr, page_type(rp), rcells, 0);
        pager.write_page(rid, std::move(newr.b));
        Page newn(pager.page_size());
        rebuild_page(newn, page_type(node), ncells, 0);
        pager.write_page(nid, std::move(newn.b));
    } else {
        // node gains (node.right, parent separator); the right sibling's
        // first cell dissolves: child -> node's new right pointer, key promoted
        ParsedCell<K> first = rcells.front();
        rcells.erase(rcells.begin());
        ncells.push_back(make_interior_parsed<K>(right_child(node),
                                                 pcells[size_t(slot)].cd.key));
        pcells[size_t(slot)] = make_interior_parsed<K>(nid, first.cd.key);
        Page newn(pager.page_size());
        rebuild_page(newn, page_type(node), ncells, first.cd.child);
        pager.write_page(nid, std::move(newn.b));
        Page newr(pager.page_size());
        rebuild_page(newr, page_type(rp), rcells, right_child(rp));
        pager.write_page(rid, std::move(newr.b));
    }
    Page pp(pager.page_size());
    rebuild_page(pp, page_type(parent), pcells, parent_right);
    pager.write_page(parent_id, std::move(pp.b));
}

// Fix underflows after a deletion. `path` holds ancestors of `node_id`.
template <class K>
void rebalance_after_delete(std::vector<std::pair<uint32_t, int>>& path, uint32_t node_id,
                            Pager& pager) {
    for (;;) {
        Page node = load_node<K>(pager, node_id);
        PageType ntype = page_type(node);
        bool leaf = page_type_is_leaf(ntype);

        if (path.empty()) {
            // root: collapse interior roots that lost all cells
            if (!leaf && cell_count(node) == 0) {
                uint32_t child = right_child(node);
                Page cp = load_node<K>(pager, child);
                Page np(pager.page_size());
                rebuild_page(np, page_type(cp), parse_all_cells<K>(cp), right_child(cp));
                pager.write_page(node_id, std::move(np.b));
                pager.free_page(child);
            }
            return;
        }
        if (cell_count(node) >= 2) return;  // healthy

        auto [pid, slot] = path.back();
        Page parent = load_node<K>(pager, pid);
        std::vector<ParsedCell<K>> pcells = parse_all_cells<K>(parent);
        uint32_t parent_right = right_child(parent);
        auto child_at = [&](int s) -> uint32_t {
            return s < int(pcells.size()) ? pcells[size_t(s)].cd.child : parent_right;
        };

        bool has_left = slot > 0;
        bool has_right = slot + 1 <= int(pcells.size());

        // ---- try borrowing from a sibling with cells to spare ----
        // A lender must keep >= 1 cell (hard) and ideally >= 2 (soft), so
        // the normal threshold is 3; a node that lost its last cell may
        // borrow from a 2-cell sibling, leaving both at 1.
        size_t lend_threshold = cell_count(node) == 0 ? 2 : 3;
        if (has_left) {
            Page lp = load_node<K>(pager, child_at(slot - 1));
            if (cell_count(lp) >= lend_threshold) {
                // the borrowed cell must actually fit next to the node's
                // own cells (oversized payloads can defeat a borrow)
                std::vector<ParsedCell<K>> try_cells = parse_all_cells<K>(node);
                try_cells.insert(try_cells.begin(), parse_all_cells<K>(lp).back());
                if (list_fits(pager.page_size(), try_cells)) {
                    borrow_from_left<K>(pager, pid, slot, node, lp);
                    return;
                }
            }
        }
        if (has_right) {
            Page rp = load_node<K>(pager, child_at(slot + 1));
            if (cell_count(rp) >= lend_threshold) {
                std::vector<ParsedCell<K>> try_cells = parse_all_cells<K>(node);
                std::vector<ParsedCell<K>> rcs = parse_all_cells<K>(rp);
                try_cells.insert(try_cells.end(), rcs.front());
                if (list_fits(pager.page_size(), try_cells)) {
                    borrow_from_right<K>(pager, pid, slot, node, rp);
                    return;
                }
            }
        }

        // ---- try merging ----
        bool merged = false;
        bool parent_lost_cell = false;

        if (has_left) {
            uint32_t lid = child_at(slot - 1);
            Page lp = load_node<K>(pager, lid);
            std::vector<ParsedCell<K>> lcells = parse_all_cells<K>(lp);
            std::vector<ParsedCell<K>> ncells = parse_all_cells<K>(node);

            std::vector<ParsedCell<K>> combined = lcells;
            uint32_t new_right = right_child(lp);
            if (!leaf) {
                combined.push_back(
                    make_interior_parsed<K>(right_child(lp), pcells[size_t(slot - 1)].cd.key));
                new_right = right_child(node);
            }
            combined.insert(combined.end(), ncells.begin(), ncells.end());

            if (list_fits(pager.page_size(), combined)) {
                K merged_max = ncells.empty() ? max_of_node<K>(pager, lp)
                                              : max_of_node<K>(pager, node);
                Page newl(pager.page_size());
                rebuild_page(newl, ntype, combined, new_right);
                pager.write_page(lid, std::move(newl.b));
                if (slot < int(pcells.size())) {
                    // node was referenced by cell `slot`: refresh the left
                    // sibling's entry and drop the node's
                    pcells[size_t(slot - 1)] =
                        make_interior_parsed<K>(lid, merged_max);
                    pcells.erase(pcells.begin() + slot);
                } else {
                    // node was the right-most child: the merged node takes
                    // its place as the right-most child, so the left
                    // sibling loses its cell (keeping it would reference
                    // the same page twice)
                    pcells.erase(pcells.begin() + (slot - 1));
                    parent_right = lid;
                }
                parent_lost_cell = true;
                pager.free_page(node_id);
                merged = true;
            }
        }

        if (!merged && has_right) {
            uint32_t rid = child_at(slot + 1);
            Page rp = load_node<K>(pager, rid);
            std::vector<ParsedCell<K>> rcells = parse_all_cells<K>(rp);
            std::vector<ParsedCell<K>> ncells = parse_all_cells<K>(node);

            std::vector<ParsedCell<K>> combined = ncells;
            uint32_t new_right = right_child(node);
            if (!leaf) {
                combined.push_back(
                    make_interior_parsed<K>(right_child(node), pcells[size_t(slot)].cd.key));
                new_right = right_child(rp);
            }
            combined.insert(combined.end(), rcells.begin(), rcells.end());

            if (list_fits(pager.page_size(), combined)) {
                K merged_max = max_of_node<K>(pager, rp);
                Page newn(pager.page_size());
                rebuild_page(newn, ntype, combined, new_right);
                pager.write_page(node_id, std::move(newn.b));
                if (slot + 1 < int(pcells.size())) {
                    // the right sibling was referenced by cell `slot + 1`:
                    // refresh the node's entry and drop the sibling's
                    pcells[size_t(slot)] =
                        make_interior_parsed<K>(node_id, merged_max);
                    pcells.erase(pcells.begin() + slot + 1);
                } else {
                    // the right sibling was the right-most child: the
                    // merged node takes its place as the right-most child
                    // and loses its own cell (keeping it would reference
                    // the same page twice)
                    pcells.erase(pcells.begin() + slot);
                    parent_right = node_id;
                }
                parent_lost_cell = true;
                pager.free_page(rid);
                merged = true;
            }
        }

        if (!merged) {
            // Pathological case (cells too large to merge): leave the node
            // underfull. The hard invariant (>= 1 cell per non-root node)
            // still holds: an empty node always fits into a merge.
            return;
        }

        {
            Page pp(pager.page_size());
            rebuild_page(pp, page_type(parent), pcells, parent_right);
            pager.write_page(pid, std::move(pp.b));
        }
        if (!parent_lost_cell) return;
        path.pop_back();
        node_id = pid;
    }
}

} // namespace btree_impl

// =====================================================================
// BTree members
// =====================================================================

using namespace btree_impl;

template <class K>
uint32_t BTree<K>::create(Pager& pager, uint32_t forced_root) {
    uint32_t id;
    if (forced_root != 0) {
        if (forced_root >= pager.page_count())
            throw DbError::pager(str("forced root page ", forced_root, " does not exist"));
        id = forced_root;
    } else {
        id = pager.allocate_page();
    }
    Page p(pager.page_size());
    init_page(p, leaf_type_for<K>());
    pager.write_page(id, std::move(p.b));
    return id;
}

template <class K>
bool BTree<K>::search(const K& key, std::vector<uint8_t>& payload_out) {
    uint32_t cur = root_;
    for (;;) {
        Page p = load_node<K>(pager_, cur);
        PageType t = page_type(p);
        if (t == leaf_type_for<K>()) {
            bool found;
            int idx = leaf_lower_bound<K>(p, key, &found);
            if (!found) return false;
            CellData<K> cd = parse_cell<K>(p, t, idx);
            if constexpr (K::node_kind == NodeKind::Table) {
                payload_out = cd.has_overflow
                                  ? read_full_payload(pager_, cd.local, cd.overflow,
                                                      cd.payload_len)
                                  : std::move(cd.local);
            } else {
                payload_out.clear();
            }
            return true;
        }
        int slot = interior_slot<K>(p, key);
        cur = slot < int(cell_count(p)) ? parse_cell<K>(p, t, slot).child : right_child(p);
    }
}

template <class K>
bool BTree<K>::max_key(K& out) {
    uint32_t cur = root_;
    for (;;) {
        Page p = load_node<K>(pager_, cur);
        if (page_type_is_leaf(page_type(p))) {
            if (cell_count(p) == 0) return false;
            out = parse_cell<K>(p, page_type(p), int(cell_count(p)) - 1).key;
            return true;
        }
        cur = right_child(p);
    }
}

template <class K>
bool BTree<K>::first_key(K& out) {
    uint32_t cur = root_;
    for (;;) {
        Page p = load_node<K>(pager_, cur);
        if (page_type_is_leaf(page_type(p))) {
            if (cell_count(p) == 0) return false;
            out = parse_cell<K>(p, page_type(p), 0).key;
            return true;
        }
        cur = parse_cell<K>(p, page_type(p), 0).child;
    }
}

template <class K>
typename BTree<K>::InsertResult BTree<K>::insert(const K& key,
                                                 const std::vector<uint8_t>* payload) {
    if constexpr (K::node_kind == NodeKind::Table) {
        if (payload == nullptr)
            throw DbError::btree("internal error: table insert requires a payload");
    } else {
        if (payload != nullptr)
            throw DbError::btree("internal error: index insert takes no payload");
    }

    // descend to the leaf, remembering the path (page, child slot)
    std::vector<std::pair<uint32_t, int>> path;
    uint32_t cur = root_;
    Page page = load_node<K>(pager_, cur);
    while (page_type(page) != leaf_type_for<K>()) {
        int slot = interior_slot<K>(page, key);
        path.emplace_back(cur, slot);
        cur = slot < int(cell_count(page))
                  ? parse_cell<K>(page, page_type(page), slot).child
                  : right_child(page);
        page = load_node<K>(pager_, cur);
    }

    bool exists;
    int idx = leaf_lower_bound<K>(page, key, &exists);
    if (exists) return InsertResult::Duplicate;

    std::vector<uint8_t> cell = build_leaf_cell<K>(pager_, key, payload);

    // fast path: fits directly (or after compaction)
    if (cell.size() <= free_contiguous(page)) {
        insert_cell(page, idx, cell.data(), cell.size());
        pager_.write_page(cur, std::move(page.b));
        return InsertResult::Inserted;
    }
    if (garbage_bytes(page) > 0) {
        std::vector<ParsedCell<K>> cs = parse_all_cells<K>(page);
        rebuild_page(page, page_type(page), cs);
        if (cell.size() <= free_contiguous(page)) {
            insert_cell(page, idx, cell.data(), cell.size());
            pager_.write_page(cur, std::move(page.b));
            return InsertResult::Inserted;
        }
    }

    // slow path: split cascade with the combined cell list
    std::vector<ParsedCell<K>> cells = parse_all_cells<K>(page);
    ParsedCell<K> newc;
    newc.cd.key = key;
    newc.bytes = std::move(cell);
    cells.insert(cells.begin() + idx, std::move(newc));

    insert_cascade<K>(path, cur, page_type(page), std::move(cells), 0, pager_);
    return InsertResult::Inserted;
}

template <class K>
bool BTree<K>::remove(const K& key) {
    std::vector<std::pair<uint32_t, int>> path;
    uint32_t cur = root_;
    Page page = load_node<K>(pager_, cur);
    while (page_type(page) != leaf_type_for<K>()) {
        int slot = interior_slot<K>(page, key);
        path.emplace_back(cur, slot);
        cur = slot < int(cell_count(page))
                  ? parse_cell<K>(page, page_type(page), slot).child
                  : right_child(page);
        page = load_node<K>(pager_, cur);
    }

    bool found;
    int idx = leaf_lower_bound<K>(page, key, &found);
    if (!found) return false;

    CellData<K> cd = parse_cell<K>(page, page_type(page), idx);
    remove_cell(page, idx, cd.size);
    if (cd.has_overflow) free_overflow_chain(pager_, cd.overflow);

    // Reclaim space eagerly: once garbage accumulates past a fraction of
    // the page, rebuild the leaf in place from its live cells (resets the
    // garbage counter). One-page cost; keeps DELETE-heavy workloads from
    // hoarding space in half-empty pages.
    if (garbage_bytes(page) > pager_.page_size() / 8)
        rebuild_page(page, page_type(page), parse_all_cells<K>(page));

    // If the leaf's maximum changed, separator keys along the rightmost
    // path may be stale (unless the leaf is now empty — merging fixes it).
    if (cell_count(page) > 0 && idx == int(cell_count(page))) {
        K new_max = parse_cell<K>(page, page_type(page), int(cell_count(page)) - 1).key;
        for (size_t i = path.size(); i-- > 0;) {
            auto [pid, slot] = path[i];
            Page parent = load_node<K>(pager_, pid);
            if (slot < int(cell_count(parent))) {
                std::vector<ParsedCell<K>> pcells = parse_all_cells<K>(parent);
                if (pcells[size_t(slot)].cd.key.compare(new_max) != 0) {
                    uint32_t child = pcells[size_t(slot)].cd.child;
                    pcells[size_t(slot)] = make_interior_parsed<K>(child, new_max);
                    Page pp(pager_.page_size());
                    rebuild_page(pp, page_type(parent), pcells, right_child(parent));
                    pager_.write_page(pid, std::move(pp.b));
                }
                break;  // only the immediate separator can be stale
            }
            // rightmost child: the parent's own maximum changed; keep walking
        }
    }

    pager_.write_page(cur, std::move(page.b));
    rebalance_after_delete<K>(path, cur, pager_);
    return true;
}

template <class K>
bool BTree<K>::update_payload(const K& key, const std::vector<uint8_t>& payload) {
    if constexpr (K::node_kind == NodeKind::Index) {
        (void)key;
        (void)payload;
        throw DbError::btree("internal error: index trees have no payload to update");
    } else {
        std::vector<uint8_t> old;
        if (!search(key, old)) return false;
        if (!remove(key)) return false;
        insert(key, &payload);
        return true;
    }
}

// =====================================================================
// Cursor
// =====================================================================

template <class K>
BTree<K>::Cursor::Cursor(Pager& pager, uint32_t root) : pager_(pager), root_(root) {
    descend_leftmost(root_);
}

template <class K>
void BTree<K>::Cursor::descend_leftmost(uint32_t page_id) {
    uint32_t cur = page_id;
    for (;;) {
        Page p = load_node<K>(pager_, cur);
        if (page_type_is_leaf(page_type(p))) {
            stack_.emplace_back(cur, 0);
            return;
        }
        stack_.emplace_back(cur, 0);
        cur = parse_cell<K>(p, page_type(p), 0).child;
    }
}

template <class K>
void BTree<K>::Cursor::seek(const K& key, bool inclusive) {
    stack_.clear();
    uint32_t cur = root_;
    for (;;) {
        Page p = load_node<K>(pager_, cur);
        if (page_type_is_leaf(page_type(p))) {
            bool found;
            int idx = leaf_lower_bound<K>(p, key, &found);
            if (!inclusive && found) idx++;
            stack_.emplace_back(cur, idx);
            return;
        }
        int slot = interior_slot<K>(p, key);
        stack_.emplace_back(cur, slot);
        cur = slot < int(cell_count(p)) ? parse_cell<K>(p, page_type(p), slot).child
                                        : right_child(p);
    }
}

template <class K>
bool BTree<K>::Cursor::next(Entry& out) {
    while (!stack_.empty()) {
        auto [leaf_id, idx] = stack_.back();
        Page leaf = load_node<K>(pager_, leaf_id);
        if (idx < int(cell_count(leaf))) {
            stack_.back().second = idx + 1;
            CellData<K> cd = parse_cell<K>(leaf, page_type(leaf), idx);
            out.key = cd.key;
            if constexpr (K::node_kind == NodeKind::Table) {
                out.payload = cd.has_overflow
                                  ? read_full_payload(pager_, cd.local, cd.overflow,
                                                      cd.payload_len)
                                  : std::move(cd.local);
            } else {
                out.payload.clear();
            }
            return true;
        }
        // leaf exhausted: advance to the next leaf
        stack_.pop_back();
        bool descended = false;
        while (!stack_.empty()) {
            auto& frame = stack_.back();
            Page parent = load_node<K>(pager_, frame.first);
            if (frame.second < int(cell_count(parent))) {
                frame.second += 1;  // enter the next child
                uint32_t child = frame.second < int(cell_count(parent))
                                     ? parse_cell<K>(parent, page_type(parent),
                                                     frame.second)
                                           .child
                                     : right_child(parent);
                descend_leftmost(child);
                descended = true;
                break;
            }
            stack_.pop_back();  // this subtree is exhausted too
        }
        if (!descended && stack_.empty()) return false;
    }
    return false;
}

// =====================================================================
// Validation / rendering / height
// =====================================================================

template <class K>
ValidationReport<K> BTree<K>::validate() {
    struct Frame {
        uint32_t page;
        uint32_t depth;
        bool has_lo = false, has_hi = false;
        K lo{}, hi{};
    };
    ValidationReport<K> rep;
    std::vector<Frame> stack;
    std::unordered_map<uint32_t, uint32_t> visited;
    std::set<uint32_t> leaf_depths;

    stack.push_back(Frame{root_, 0});

    while (!stack.empty()) {
        Frame f = stack.back();
        stack.pop_back();

        if (f.page >= pager_.page_count())
            throw DbError::btree(
                str("child reference to page ", f.page, " beyond the end of the file"));
        if (visited.count(f.page))
            throw DbError::btree(
                str("page ", f.page, " is referenced twice (cycle or shared subtree)"));
        visited[f.page] = f.depth;

        Page p = load_node<K>(pager_, f.page);
        PageType t = page_type(p);
        bool leaf = page_type_is_leaf(t);
        ++rep.pages;
        rep.page_ids.push_back(f.page);

        // parse cells, check ordering and range bounds
        std::vector<CellData<K>> cds;
        cds.reserve(cell_count(p));
        for (int i = 0; i < int(cell_count(p)); ++i) {
            CellData<K> cd = parse_cell<K>(p, t, i);
            if (!cds.empty() && cds.back().key.compare(cd.key) >= 0)
                throw DbError::btree(str("page ", f.page, ": cells ", i - 1, " and ", i,
                                         " are out of order or duplicated (",
                                         cds.back().key.display(), " >= ",
                                         cd.key.display(), ")"));
            if (leaf) {
                if (f.has_lo && !(f.lo.compare(cd.key) < 0))
                    throw DbError::btree(str("page ", f.page, ": key ", cd.key.display(),
                                             " violates the lower bound of its position"));
                if (f.has_hi && !(cd.key.compare(f.hi) <= 0))
                    throw DbError::btree(str("page ", f.page, ": key ", cd.key.display(),
                                             " violates the upper bound of its position"));
                if (cd.has_overflow) {
                    std::vector<uint32_t> chain;
                    validate_overflow_chain(pager_, cd.overflow, cd.payload_len,
                                            cd.local.size(), &chain);
                    for (uint32_t op : chain) {
                        if (visited.count(op))
                            throw DbError::btree(str(
                                "overflow page ", op, " is shared or is also a "
                                "tree node"));
                        visited[op] = f.depth;
                        rep.page_ids.push_back(op);
                        ++rep.pages;
                    }
                }
            }
            cds.push_back(std::move(cd));
        }

        if (leaf) {
            if (f.depth > 0 && cds.empty())
                throw DbError::btree(str("non-root leaf page ", f.page, " is empty"));
            if (f.depth > 0 && cds.size() < 2)
                rep.warnings.push_back(str("page ", f.page, " holds only ", cds.size(),
                                           " cell(s) (below the soft minimum of 2)"));
            if (!cds.empty()) {
                leaf_depths.insert(f.depth);
                if (leaf_depths.size() > 1)
                    throw DbError::btree(
                        "leaves exist at different depths (unbalanced tree)");
                rep.height = std::max(rep.height, f.depth + 1);
            }
            rep.leaves += 1;
            rep.keys += cds.size();
        } else {
            if (cds.empty())
                throw DbError::btree(str("interior page ", f.page,
                                         " has no cells (the root should have collapsed)"));
            for (int i = 0; i < int(cds.size()); ++i) {
                if (cds[size_t(i)].child == 0)
                    throw DbError::btree(
                        str("page ", f.page, ", cell ", i, ": child pointer is 0"));
                if (cds[size_t(i)].child == f.page)
                    throw DbError::btree(
                        str("page ", f.page, ", cell ", i, ": node points to itself"));
            }
            if (right_child(p) == 0 || right_child(p) == f.page)
                throw DbError::btree(
                    str("page ", f.page, ": invalid right-most pointer"));

            // push children (right to left so processing is left to right)
            for (int i = int(cds.size()); i-- > 0;) {
                Frame cf;
                cf.page = cds[size_t(i)].child;
                cf.depth = f.depth + 1;
                if (i > 0) {
                    cf.has_lo = true;
                    cf.lo = cds[size_t(i - 1)].key;
                } else {
                    cf.has_lo = f.has_lo;
                    cf.lo = f.lo;
                }
                cf.has_hi = true;
                cf.hi = cds[size_t(i)].key;
                stack.push_back(std::move(cf));
            }
            Frame rf;
            rf.page = right_child(p);
            rf.depth = f.depth + 1;
            rf.has_lo = true;
            rf.lo = cds.back().key;
            rf.has_hi = f.has_hi;
            rf.hi = f.hi;
            stack.push_back(std::move(rf));
        }
    }
    if (rep.keys == 0) rep.height = 1;
    return rep;
}

template <class K>
std::string BTree<K>::render() {
    std::ostringstream out;
    struct Frame {
        uint32_t page;
        int depth;
    };
    std::vector<Frame> stack{{root_, 0}};
    size_t lines = 0;
    while (!stack.empty() && lines < 400) {
        Frame f = stack.back();
        stack.pop_back();
        Page p = load_node<K>(pager_, f.page);
        PageType t = page_type(p);
        out << std::string(size_t(f.depth) * 2, ' ') << "page " << f.page << " ["
            << page_type_name(t) << "] cells=" << cell_count(p);
        if (cell_count(p) > 0) {
            CellData<K> first = parse_cell<K>(p, t, 0);
            CellData<K> last = parse_cell<K>(p, t, int(cell_count(p)) - 1);
            out << " keys " << first.key.display() << " .. " << last.key.display();
        }
        out << "\n";
        ++lines;
        if (!page_type_is_leaf(t)) {
            std::vector<uint32_t> children;
            for (int i = 0; i < int(cell_count(p)); ++i)
                children.push_back(parse_cell<K>(p, t, i).child);
            children.push_back(right_child(p));
            for (size_t i = children.size(); i-- > 0;)
                stack.push_back(Frame{children[i], f.depth + 1});
        }
    }
    return out.str();
}

template <class K>
uint32_t BTree<K>::height() {
    uint32_t h = 1;
    uint32_t cur = root_;
    for (;;) {
        Page p = load_node<K>(pager_, cur);
        if (page_type_is_leaf(page_type(p))) return h;
        ++h;
        cur = right_child(p);
    }
}

// explicit instantiations for the two key kinds
template class BTree<RowIdKey>;
template class BTree<IndexKey>;

} // namespace sc
