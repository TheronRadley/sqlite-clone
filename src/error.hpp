// error.hpp — structured, actionable errors.
//
// Every subsystem raises a DbError with a specific code and a message that
// says *what* went wrong and *where*. Errors are never swallowed; the CLI
// catches them at the statement boundary and prints them.
#pragma once

#include <exception>
#include <string>
#include <utility>

namespace sc {

enum class Err {
    Lex,          // tokenizer failure
    Parse,        // parser failure
    Type,         // value/type mismatch
    Constraint,   // schema or key constraint violation
    Storage,      // on-disk format / file-level failure
    Pager,        // page-level I/O or cache failure
    BTree,        // structural B-Tree failure (incl. detected corruption)
    Transaction,  // transaction protocol misuse or failure
    Execution,    // execution planning / semantic failure
};

inline const char* err_name(Err e) {
    switch (e) {
        case Err::Lex:         return "LexError";
        case Err::Parse:       return "ParseError";
        case Err::Type:        return "TypeError";
        case Err::Constraint:  return "ConstraintError";
        case Err::Storage:     return "StorageError";
        case Err::Pager:       return "PagerError";
        case Err::BTree:       return "BTreeError";
        case Err::Transaction: return "TransactionError";
        case Err::Execution:   return "ExecutionError";
    }
    return "?";
}

struct DbError : std::exception {
    Err code;
    std::string message;

    DbError(Err c, std::string m) : code(c), message(std::move(m)) {}
    const char* what() const noexcept override { return message.c_str(); }

    // ---- factories with the required message shapes ----

    static DbError lex(int line, int col, const std::string& m) {
        return DbError(Err::Lex, m + " at line " + std::to_string(line) +
                                     ", column " + std::to_string(col));
    }
    static DbError parse(int line, int col, const std::string& expected,
                         const std::string& found) {
        return DbError(Err::Parse, "parse error at line " + std::to_string(line) +
                                       ", column " + std::to_string(col) + ": expected " +
                                       expected + ", found " + found);
    }
    static DbError type(const std::string& m) { return DbError(Err::Type, m); }
    static DbError constraint(const std::string& m) { return DbError(Err::Constraint, m); }
    static DbError storage(const std::string& m) { return DbError(Err::Storage, m); }
    static DbError pager(const std::string& m) { return DbError(Err::Pager, m); }
    static DbError btree(const std::string& m) { return DbError(Err::BTree, m); }
    static DbError txn(const std::string& m) { return DbError(Err::Transaction, m); }
    static DbError exec(const std::string& m) { return DbError(Err::Execution, m); }
};

[[noreturn]] inline void raise(Err c, std::string m) { throw DbError(c, std::move(m)); }

} // namespace sc
