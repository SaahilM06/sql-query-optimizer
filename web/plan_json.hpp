#pragma once

#include "../execution/executor.hpp"
#include "../physical/physical_plan.hpp"
#include "../util/json.hpp"

namespace sql::web {

// Builds a JSON view of `plan`, recursively -- one object per node with
// "operator", "estimated_rows", "estimated_cost", "reasoning", an optional
// "table", and a "children" array. If `executor` is non-null (the query
// was actually executed, via the same build_executor(plan, ...) tree used
// elsewhere), each node also gets "actual_rows"/"actual_time_ms" from that
// executor node -- the JSON counterpart to execution::explain_analyze's
// text tree, used by both the planned-only and executed API responses so
// the frontend has one tree shape to render either way.
sql::util::JsonValue plan_to_json_tree(const sql::physical::PhysicalPlan& plan, sql::execution::Executor* executor);

} // namespace sql::web
