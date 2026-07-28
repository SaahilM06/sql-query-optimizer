#pragma once

#include "../logical/schema.hpp"
#include "../physical/physical_plan.hpp"
#include "../statistics/statistics_catalog.hpp"
#include "cardinality_estimator.hpp"
#include "join_graph.hpp"

namespace sql::optimizer {

// Builds the best base access path (SeqScan, or IndexScan when one local
// filter is an equality on an indexed column) for one Relation, folding
// in every one of its local_filters.
//
// This is the join-search subsystem's own version of the single-table
// scan/filter logic PhysicalPlanner already has (plan_scan/try_index_scan
// in physical/physical_planner.cpp) -- kept as a separate, self-contained
// implementation per the spec's module layout rather than reaching into
// PhysicalPlanner's private methods, since it needs to handle a Relation
// with an arbitrary *list* of local_filters rather than a single Filter
// node wrapping a Scan.
class AccessPathGenerator {
public:
    AccessPathGenerator(const sql::logical::Catalog& schema_catalog,
                         const sql::statistics::StatisticsCatalog& stats_catalog, const CardinalityEstimator& cardinality)
        : schema_catalog_(schema_catalog), stats_catalog_(stats_catalog), cardinality_(cardinality) {}

    sql::physical::PhysicalPlan best_access_path(const Relation& rel) const;

private:
    const sql::logical::Catalog& schema_catalog_;
    const sql::statistics::StatisticsCatalog& stats_catalog_;
    const CardinalityEstimator& cardinality_;
};

} // namespace sql::optimizer
