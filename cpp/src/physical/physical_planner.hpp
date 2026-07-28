#pragma once

#include <optional>

#include "../logical/logical_plan.hpp"
#include "../logical/schema.hpp"
#include "../optimizer/cardinality_estimator.hpp"
#include "../parser/ast.hpp"
#include "../statistics/statistics_catalog.hpp"
#include "physical_plan.hpp"

namespace sql::physical {

using sql::logical::Catalog;
using sql::logical::LogicalPlan;
using sql::optimizer::CardinalityEstimator;
using sql::parser::Expression;
using sql::parser::JoinType;
using sql::statistics::StatisticsCatalog;

// Translates an (already logically-optimized) LogicalPlan into a PhysicalPlan
// by picking, at each node, the cheaper of the available execution
// strategies according to the cost model in cost.hpp/.cpp, using row
// estimates from CardinalityEstimator (backed by real table/column
// statistics where available) rather than fixed heuristics:
//
//   Scan:  SeqScan, or IndexScan when a Filter directly above the scan is an
//          equality predicate on an indexed column and indexing wins on cost.
//   Join:  NestedLoopJoin always; HashJoin too when the condition is an
//          equi-join, keeping whichever is cheaper.
//
// Every other node (Filter that didn't collapse into an IndexScan, Aggregate,
// Project, Sort, Limit) has exactly one physical strategy in this version --
// only scan/join strategy selection is modeled so far.
//
// `schema_catalog` supplies index metadata (which columns can support an
// IndexScan); `stats_catalog` supplies the row counts, distinct counts, and
// histograms CardinalityEstimator needs. These are deliberately two
// different catalogs -- see statistics_catalog.hpp for why.
class PhysicalPlanner {
public:
    PhysicalPlanner(const Catalog& schema_catalog, const StatisticsCatalog& stats_catalog)
        : schema_catalog_(schema_catalog), stats_catalog_(stats_catalog), cardinality_(stats_catalog) {}

    PhysicalPlan plan(const LogicalPlan& logical);

private:
    const Catalog& schema_catalog_;
    const StatisticsCatalog& stats_catalog_;
    CardinalityEstimator cardinality_;

    PhysicalPlan plan_node(const LogicalPlan& node);
    PhysicalPlan plan_scan(const LogicalPlan& scan);
    std::optional<PhysicalPlan> try_index_scan(const LogicalPlan& filter_node, const LogicalPlan& scan_node);
    PhysicalPlan choose_join_strategy(JoinType join_type, Expression condition, PhysicalPlan left, PhysicalPlan right,
                                       const LogicalPlan& left_scope, const LogicalPlan& right_scope);
};

/// Convenience one-shot entry point.
PhysicalPlan generate_physical_plan(const LogicalPlan& logical, const Catalog& schema_catalog,
                                     const StatisticsCatalog& stats_catalog);

} // namespace sql::physical
