// database.hpp — the engine facade: one open database file.
//
// A Database owns the pager and the catalog and drives the transaction
// protocol around every statement:
//
//   * outside BEGIN...COMMIT every statement is its own transaction
//     (autocommit): BEGIN -> run -> COMMIT, or BEGIN -> run -> ROLLBACK
//     on error
//   * inside an explicit transaction each statement runs with a savepoint;
//     a failed statement rolls back to the savepoint, the transaction
//     survives
//
// It also exposes the introspection the CLI needs (.tables, .schema,
// .dump, .pages, .btree, .validate).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "catalog.hpp"
#include "executor.hpp"
#include "pager.hpp"

namespace sc {

class Database {
public:
    // Open (creating if necessary) a file-backed database. Runs rollback
    // journal recovery if a hot journal is present.
    static std::unique_ptr<Database> open(const std::string& path,
                                          uint32_t page_size = 4096);
    // Fully in-memory database (same engine, no file).
    static std::unique_ptr<Database> open_memory(uint32_t page_size = 4096);
    ~Database();

    // Parse and execute one or more ';'-separated statements. Throws
    // DbError subclasses; on failure the statement's changes are undone.
    std::vector<ExecResult> execute(const std::string& sql);

    bool in_txn() const { return pager_->in_txn(); }
    const Pager& pager() const { return *pager_; }
    Catalog& catalog() { return *catalog_; }

    // Mutable pager access for tools (the CLI's .cache tuning, the
    // benchmark harness). Normal query execution must go through
    // execute(); touching pager pages directly bypasses transactions.
    Pager& pager_for_tools() { return *pager_; }

    // ---- introspection (CLI / tests) ----
    std::vector<std::string> table_names() const;
    std::vector<std::string> index_names() const;
    std::string schema_sql() const;
    std::string dump_sql();

    struct PageInfo {
        uint32_t id;
        std::string kind;
    };
    std::vector<PageInfo> page_inventory();

    std::string render_btree(const std::string& table_or_index);

    // Full structural + (deep=true) index/table consistency validation.
    // Throws DbError describing the first problem found.
    void validate(bool deep);

private:
    explicit Database(std::unique_ptr<Pager> pager);

    std::unique_ptr<Pager> pager_;
    std::unique_ptr<Catalog> catalog_;
};

} // namespace sc
