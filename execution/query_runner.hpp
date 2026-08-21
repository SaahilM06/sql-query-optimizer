#pragma once

#include <ostream>
#include <vector>

#include "../physical/physical_plan.hpp"
#include "executor.hpp"

namespace sql::execution {

struct ExecutionResult {
    std::vector<sql::storage::Row> rows;
    double total_elapsed_ms = 0.0;
};

// Fully drains `root` (open -> next* -> close) and collects every row.
ExecutionResult run_to_completion(Executor& root);

// Runs `executor` to completion and prints a combined estimated/actual
// tree: each PhysicalPlan node's existing estimated_rows/estimated_cost
// next to the matching Executor node's actual rows_produced()/elapsed_ms().
// `plan` and `executor` must be the same tree shape -- i.e. `executor` came
// from build_executor(plan, ...) (see executor_builder.hpp). Returns the
// same ExecutionResult run_to_completion would, so callers that also want
// to log/report the overall row count and timing (not just print the tree)
// don't have to execute twice.
ExecutionResult explain_analyze(const sql::physical::PhysicalPlan& plan, Executor& executor, std::ostream& os);

} // namespace sql::execution
