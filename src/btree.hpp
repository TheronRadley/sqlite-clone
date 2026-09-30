// btree.hpp — the disk-backed B+-Tree.
//
// One template implements both tree kinds (see keys.hpp):
//   * table trees (K = RowIdKey): keys are row ids; leaf cells carry the
//     serialized row as payload; payloads may spill into overflow pages
//   * index trees (K = IndexKey): keys are (value, rowid) pairs; there is
//     no payload — the key IS the entry
//
// Structure (see B_TREE.md):
//   * interior cells are (child page, separator key). Child i of a node
//     holds all keys in (K_{i-1}, K_i]; the right-most pointer holds all
//     keys > K_last. This is SQLite's routing convention.
//   * all entries live in leaves; interior keys are pure separators and
//     may go stale after deletions (routing stays correct).
//   * the root page id never changes: when the root splits, its content
//     moves to a fresh child and the root is re-initialized as interior.
//   * nodes rebalance on delete: borrow from a sibling, else merge. Hard
//     invariant: every non-root node holds >= 1 cell. The soft target is
//     >= 2 cells (reported as a warning by the validator when violated,
//     which can happen when two near-page-sized cells make a merge
//     impossible).
//
// This layer knows nothing about SQL, rows, or schemas. It operates on
// encoded keys and opaque payload bytes through the pager.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "keys.hpp"
#include "page.hpp"
#include "pager.hpp"

namespace sc {

template <class K>
struct CellData {
    K key{};
    uint32_t child = 0;            // interior cells
    uint32_t payload_len = 0;      // table-leaf cells: total payload bytes
    std::vector<uint8_t> local;    // table-leaf cells: inline payload bytes
    uint32_t overflow = 0;         // table-leaf cells: first overflow page
    bool has_overflow = false;
    size_t size = 0;               // total bytes this cell occupies in-page
};

template <class K>
struct ValidationReport {
    uint64_t pages = 0, leaves = 0, keys = 0;
    uint32_t height = 0;
    std::vector<std::string> warnings;
    std::vector<uint32_t> page_ids;   // every page owned by this tree
};

template <class K>
class BTree {
public:
    BTree(Pager& pager, uint32_t root) : pager_(pager), root_(root) {}

    NodeKind kind() const { return K::node_kind; }
    uint32_t root() const { return root_; }

    // Initialize a new empty tree. If forced_root != 0 the tree roots at
    // that page (used for the catalog at page 1); otherwise a page is
    // allocated. Returns the root page id.
    static uint32_t create(Pager& pager, uint32_t forced_root = 0);

    enum class InsertResult { Inserted, Duplicate };
    // Insert `key`. For table trees `payload` is the row record; for index
    // trees it must be nullptr. Returns Duplicate without modifying the
    // tree if the key already exists.
    InsertResult insert(const K& key, const std::vector<uint8_t>* payload);

    // Exact-match search; fills `payload_out` (table trees).
    bool search(const K& key, std::vector<uint8_t>& payload_out);

    // Remove `key`. Returns false if absent.
    bool remove(const K& key);

    // Replace the payload of an existing key (table trees).
    bool update_payload(const K& key, const std::vector<uint8_t>& payload);

    bool max_key(K& out);    // false if the tree is empty
    bool first_key(K& out);

    // ---- ordered iteration ----
    struct Entry {
        K key;
        std::vector<uint8_t> payload;   // empty for index trees
    };

    class Cursor {
    public:
        Cursor(Pager& pager, uint32_t root);

        // Position at the first entry >= key (inclusive) or > key
        // (exclusive). Invalidates prior position.
        void seek(const K& key, bool inclusive);
        // Advance; returns false at end of tree.
        bool next(Entry& out);

    private:
        void descend_leftmost(uint32_t page_id);

        Pager& pager_;
        uint32_t root_;
        std::vector<std::pair<uint32_t, int>> stack_;  // (page, child slot)
    };

    Cursor cursor() { return Cursor(pager_, root_); }

    // Independent structural validation. Throws BTreeError describing the
    // first violation found; otherwise returns statistics + soft warnings.
    ValidationReport<K> validate();

    // Render an ASCII sketch of the tree (for the .btree command / tests).
    std::string render();

    // Height in levels (1 = root-only).
    uint32_t height();

private:
    Pager& pager_;
    uint32_t root_;
};

} // namespace sc
