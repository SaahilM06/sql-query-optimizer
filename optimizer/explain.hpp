#pragma once

#include <ostream>

#include "../physical/physical_plan.hpp"

namespace sql::optimizer {

// Prints an annotated physical plan tree: node kind, estimated rows, the
// CPU/I/O/memory cost breakdown, and (when available) the
// CardinalityEstimator's reasoning for that row estimate. This is what
// makes a surprising plan choice debuggable instead of a black box.
void explain_plan(const sql::physical::PhysicalPlan& plan, std::ostream& os, int depth = 0);

} // namespace sql::optimizer
