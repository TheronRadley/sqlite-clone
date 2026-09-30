// planner.hpp — choosing a physical access path.
//
// The planner looks at the top-level AND-conjuncts of the WHERE clause and
// decides:
//     rowid equality on the primary key  ->  RowidSeek (table B-Tree point lookup)
//     equality on an indexed column      ->  IndexSeek
//     range on an indexed column         ->  IndexRange
//     otherwise                          ->  SeqScan (full table scan)
//
// Whatever it picks, the original predicate is re-evaluated against every
// row (the "residual filter"), so a wrong choice can never change results.
// The plan choice is observable through EXPLAIN.
#pragma once

#include <string>

#include "ast.hpp"
#include "catalog.hpp"

namespace sc {

struct Plan {
    enum class Type { SeqScan, RowidSeek, IndexSeek, IndexRange };
    Type type = Type::SeqScan;

    const TableDef* table = nullptr;
    const IndexDef* index = nullptr;

    // RowidSeek
    int64_t rowid = 0;

    // IndexSeek / IndexRange
    Value value;                     // boundary value
    bool has_low = false, has_high = false;
    bool low_inclusive = true, high_inclusive = true;

    const Expr* filter = nullptr;    // residual predicate (the full WHERE)

    std::string describe() const;
};

// Choose an access path for a statement over `table` with predicate
// `where` (may be null).
Plan plan_query(const Catalog& catalog, const TableDef& table, const Expr* where);

} // namespace sc
