// executor.hpp — the boundary between the AST and physical storage.
//
// Executes one parsed statement against (Pager, Catalog). Transaction
// wrapping — autocommit and statement-level rollback — is the caller's
// job (see Database::execute). This layer never touches raw page bytes:
// it works through the B-Tree and record codecs.
#pragma once

#include "ast.hpp"
#include "catalog.hpp"
#include "pager.hpp"
#include "value.hpp"

namespace sc {

struct ExecResult {
    bool is_rows = false;
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> rows;
    std::string message;   // for statements that do not produce rows

    static ExecResult ok(std::string msg) {
        ExecResult r;
        r.message = std::move(msg);
        return r;
    }
    static ExecResult table(std::vector<std::string> cols,
                            std::vector<std::vector<Value>> rows) {
        ExecResult r;
        r.is_rows = true;
        r.columns = std::move(cols);
        r.rows = std::move(rows);
        return r;
    }
};

ExecResult execute_statement(Pager& pager, Catalog& catalog, const Statement& stmt);

} // namespace sc
