#pragma once

#include "../parser/ast.hpp"
#include "../storage/row.hpp"
#include "row_schema.hpp"

namespace sql::execution {

// Evaluates `expr` against `row` (whose columns are described by `schema`).
// Handles Column/Literal/BinaryOp/UnaryOp -- Function and Wildcard aren't
// supported here because by the time a query reaches physical planning,
// aggregate calls have already been split out into AggregateExpr (see
// logical::LogicalPlanner::split_select_list) and evaluated separately by
// HashAggregateExec, and a bare SELECT * never produces a Project node at
// all (see LogicalPlanner::plan) -- so neither ever reaches a row-expression
// tree in practice. Throws std::runtime_error if a Column can't be resolved
// or an operator hits an incompatible value kind.
sql::parser::Literal evaluate(const sql::parser::Expression& expr, const sql::storage::Row& row, const RowSchema& schema);

} // namespace sql::execution
