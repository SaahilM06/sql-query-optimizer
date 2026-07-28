#include "cost_model.hpp"

#include <stdexcept>

namespace sql::optimizer {

using namespace sql::physical;

// Mirrors PhysicalPlanner's cost composition exactly (same formulas, same
// per-join-kind composition rules), but walks an already-built tree using
// each node's already-annotated `estimated_rows` instead of computing them
// fresh from statistics. Leaf (Scan) page-count is approximated via
// cost::pages_for with a default row-size assumption, since CostModel has
// no StatisticsCatalog of its own -- PhysicalPlanner's own numbers (which
// do have real page counts) remain the authoritative source; this is a
// read-only recomputation for explain output and future re-costing.
cost::Cost CostModel::estimate_cost(const PhysicalPlan& plan) const {
    switch (plan.kind) {
        case PhysicalPlan::Kind::SeqScan: {
            double page_count = cost::pages_for(plan.estimated_rows, 64);
            return cost::seq_scan(plan.estimated_rows, page_count);
        }
        case PhysicalPlan::Kind::IndexScan:
            return cost::index_scan(plan.estimated_rows);

        case PhysicalPlan::Kind::Filter:
            return estimate_cost(*plan.input) + cost::filter(plan.input->estimated_rows);

        case PhysicalPlan::Kind::NestedLoopJoin: {
            cost::Cost c = estimate_cost(*plan.left);
            c.cpu += static_cast<double>(plan.left->estimated_rows) * estimate_cost(*plan.right).total();
            c += cost::nested_loop_join(plan.left->estimated_rows, plan.right->estimated_rows);
            return c;
        }
        case PhysicalPlan::Kind::HashJoin:
            return estimate_cost(*plan.left) + estimate_cost(*plan.right) +
                   cost::hash_join(plan.left->estimated_rows, plan.right->estimated_rows);

        case PhysicalPlan::Kind::HashAggregate:
            return estimate_cost(*plan.input) + cost::hash_aggregate(plan.input->estimated_rows, plan.estimated_rows);

        case PhysicalPlan::Kind::Project:
            return estimate_cost(*plan.input) + cost::project(plan.input->estimated_rows);

        case PhysicalPlan::Kind::Sort:
            return estimate_cost(*plan.input) + cost::sort(plan.input->estimated_rows);

        case PhysicalPlan::Kind::Limit:
            return estimate_cost(*plan.input) + cost::limit(plan.input->estimated_rows);
    }
    throw std::logic_error("unreachable: unknown PhysicalPlan::Kind");
}

} // namespace sql::optimizer
