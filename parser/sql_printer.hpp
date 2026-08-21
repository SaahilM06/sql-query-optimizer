#pragma once

#include <string>

#include "ast.hpp"

namespace sql::parser {

// Renders a parsed Statement back to a canonical SQL string: fixed keyword
// casing/spacing, deterministic clause order (the same order the parser
// requires on input). Two queries that differ only in whitespace or
// keyword case parse to the same AST and therefore print to the same
// string here -- exactly the property the plan cache's key needs, since the
// key is built by hashing this output, not the original query text.
std::string to_canonical_sql(const Statement& stmt);

} // namespace sql::parser
