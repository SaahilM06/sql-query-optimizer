#pragma once

#include <vector>

#include "../parser/ast.hpp"

namespace sql::storage {

// A materialized row of actual data. Reuses parser::Literal as the value
// type rather than inventing a parallel tagged union -- a scanned Int/Float/
// Text/Boolean/Null column value is exactly the same shape as a parsed SQL
// literal, and expression evaluation (execution/expr_eval.hpp) needs to
// combine both anyway (e.g. `total > 100` evaluates a real column value
// against a parsed literal).
using Row = std::vector<sql::parser::Literal>;

} // namespace sql::storage
