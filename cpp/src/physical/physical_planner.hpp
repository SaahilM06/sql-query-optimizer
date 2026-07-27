#pragma once

#include <optional>

#include "../logical/logical_plan.hpp"
#include "../logical/schema.hpp"
#include "../parser/ast.hpp"
#include "physical_plan.hpp"

namespace sql::physical {

using sql::logical::Catalog;
using sql::logical::LogicalPlan;
using sql::parser::Expression;
using sql::parser::JoinType;

// Translates an (already logically-optimized) LogicalPlan into a PhysicalPlan
// by picking, at each node, the cheaper of the available execution
// strategies according to the cost model in cost.hpp/.cpp:
//
//   Scan:  SeqScan, or IndexScan when a Filter directly above the scan is an
//          equality predicate on an indexed column and indexing wins on cost.
//   Join:  NestedLoopJoin always; HashJoin too when the condition is an
//          equi-join, keeping whichever is cheaper.
//
// Every other node (Filter that didn't collapse into an IndexScan, Aggregate,
// Project, Sort, Limit) has exactly one physical strategy in this version --
// only scan/join strategy selection is modeled so far.
class PhysicalPlanner {
public:
    explicit PhysicalPlanner(const Catalog& catalog) : catalog_(catalog) {}

    PhysicalPlan plan(const LogicalPlan& logical);

private:
    const Catalog& catalog_;

    PhysicalPlan plan_node(const LogicalPlan& node);
    PhysicalPlan plan_scan(const LogicalPlan& scan);
    std::optional<PhysicalPlan> try_index_scan(const LogicalPlan& filter_node, const LogicalPlan& scan_node);
    PhysicalPlan choose_join_strategy(JoinType join_type, Expression condition, PhysicalPlan left, PhysicalPlan right);

    size_t estimate_filter_rows(size_t input_rows) const;
    size_t estimate_join_rows(size_t left_rows, size_t right_rows) const;
};

/// Convenience one-shot entry point.
PhysicalPlan generate_physical_plan(const LogicalPlan& logical, const Catalog& catalog);

} // namespace sql::physical
