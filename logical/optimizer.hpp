#pragma once

#include "logical_plan.hpp"
#include "schema.hpp"

namespace sql::logical {

/// Run all three optimization passes in order: predicate pushdown,
/// projection pushdown, then join reordering.
LogicalPlan optimize(LogicalPlan plan, const Catalog& catalog);

} // namespace sql::logical
