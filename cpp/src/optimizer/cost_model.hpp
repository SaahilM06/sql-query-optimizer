#pragma once

#include "../physical/cost.hpp"
#include "../physical/physical_plan.hpp"

namespace sql::optimizer {

// Recomputes the cumulative cost of an already-built PhysicalPlan from
// scratch, using each node's `estimated_rows` (already annotated by
// PhysicalPlanner via CardinalityEstimator).
//
// PhysicalPlanner computes cost incrementally *while building* the tree,
// since it needs cost to choose between candidate strategies at each node
// (SeqScan vs IndexScan, NestedLoopJoin vs HashJoin) before the tree above
// that node even exists yet. CostModel is the complementary read-only
// operation: given a finished plan, what does it cost? That's what an
// explain/print pass needs, and it's what a future join-order search
// would use to re-cost candidate plans without re-running the whole
// planner.
class CostModel {
public:
    sql::physical::cost::Cost estimate_cost(const sql::physical::PhysicalPlan& plan) const;
};

} // namespace sql::optimizer
