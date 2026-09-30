// lexer.hpp — SQL tokenizer.
//
// Produces a flat token stream with line/column positions. Knows nothing
// about SQL semantics: the parser decides what the tokens mean.
// Supported: keywords (case-insensitive), identifiers, integer literals,
// single-quoted strings ('' escapes), double-quoted identifiers, operators,
// punctuation, and `--` / /* */ comments.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "error.hpp"
#include "util.hpp"

namespace sc {

enum class Tok {
    // keywords (the set required by the project brief, plus transaction words)
    KwSelect, KwInsert, KwUpdate, KwDelete, KwCreate, KwTable, KwIndex, KwFrom,
    KwWhere, KwAnd, KwOr, KwNot, KwOrder, KwBy, KwLimit, KwOffset, KwPrimary,
    KwKey, KwValues, KwInto, KwSet, KwAsc, KwDesc, KwNull, KwInteger, KwText,
    KwIs, KwBegin, KwCommit, KwRollback, KwTransaction, KwExplain, KwCount,
    KwMin, KwMax, KwSum, KwOn,
    // literals & names
    Ident, IntLit, StrLit,
    // operators
    OpEq, OpNe, OpLt, OpLe, OpGt, OpGe,
    OpPlus, OpMinus, OpStar, OpSlash, OpPercent,
    // punctuation
    LParen, RParen, Comma, Semicolon, Dot,
    // control
    Eof,
};

const char* tok_name(Tok t);

struct Token {
    Tok kind;
    std::string text;   // raw text (string literal text is unescaped)
    int line = 1, col = 1;

    bool is(Tok k) const { return kind == k; }
    bool is_kw(const char* word) const;  // case-insensitive keyword text match
};

// Tokenize `sql`. Throws DbError(Err::Lex) with line/column on bad input.
std::vector<Token> lex(const std::string& sql);

} // namespace sc
