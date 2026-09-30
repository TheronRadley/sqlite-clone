// parser.hpp — recursive-descent SQL parser with explicit precedence.
//
// Precedence (lowest binds loosest):
//   OR  <  AND  <  NOT  <  comparisons/IS  <  + -  <  * / %  <  unary -  <  primary
//
// The parser touches no disk and executes nothing. It only checks syntax
// and shape; semantic checks (unknown tables, arity, types) happen in the
// executor.
#pragma once

#include <memory>
#include <vector>

#include "ast.hpp"
#include "lexer.hpp"

namespace sc {

// Parse one or more ';' separated statements. Empty statements are skipped.
// Throws DbError(Err::Parse) with location, expected, and found text.
std::vector<std::unique_ptr<Statement>> parse(const std::string& sql);

} // namespace sc
