#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>

#include "../logical/schema.hpp"
#include "../physical/physical_plan.hpp"
#include "../statistics/statistics_catalog.hpp"
#include "access_path_generator.hpp"
#include "cardinality_estimator.hpp"
#include "join_graph.hpp"

namespace sql::optimizer {

// A relation subset, as a bitmask (bit i set == relations[i] is in the
// subset). Supports up to 64 relations, which is far beyond what a
// brute-force O(2^n) subset enumeration stays fast for anyway -- fine at
// this optimizer's scale (a handful of tables per query), same tradeoff
// every classic Selinger-style implementation makes before switching to a
// heuristic search for large join counts.
using RelationMask = uint64_t;

// Selinger-style dynamic-programming join-order search: builds up the
// cheapest plan for every relation subset from size 1 to the full set,
// memoizing each subset's best plan so larger subsets reuse it instead of
// recomputing. Join order and physical join algorithm are chosen
// together, one subset at a time -- not order first, algorithm second --
// since which algorithm is cheapest depends on the specific inputs being
// joined at that step.
//
// v1 scope: left-deep plans only (every candidate joins one new relation
// onto an existing subset's plan, never two multi-relation subsets
// together). Bushy plans are a documented extension, not implemented here.
class JoinEnumerator {
public:
    JoinEnumerator(const sql::logical::Catalog& schema_catalog, const sql::statistics::StatisticsCatalog& stats_catalog,
                   const CardinalityEstimator& cardinality);

    // Returns the cheapest complete join plan covering every relation in
    // `graph`. Throws std::runtime_error if the graph isn't fully
    // connected (i.e. some pair of relations would require a cartesian
    // product to join, which this search deliberately refuses to
    // generate -- see is_join_search_candidate's Milestone 3.5 note).
    sql::physical::PhysicalPlan find_best_plan(const JoinGraph& graph);

private:
    const sql::logical::Catalog& schema_catalog_;
    const CardinalityEstimator& cardinality_;
    AccessPathGenerator access_path_gen_;

    std::unordered_map<RelationMask, sql::physical::PhysicalPlan> memo_;

    void solve_subset(RelationMask subset, const JoinGraph& graph);

    bool find_connecting_edge(RelationMask left_mask, RelationMask right_mask, const JoinGraph& graph,
                               const JoinEdge** out_edge) const;

    sql::physical::PhysicalPlan generate_best_candidate(RelationMask left_mask, RelationMask right_mask,
                                                         const sql::physical::PhysicalPlan& left_plan,
                                                         const sql::physical::PhysicalPlan& right_plan,
                                                         const JoinEdge& edge, const JoinGraph& graph) const;

    std::optional<sql::physical::PhysicalPlan> try_index_nested_loop(
        RelationMask left_mask, RelationMask right_mask, const sql::physical::PhysicalPlan& left_plan,
        const sql::physical::PhysicalPlan& right_plan, const JoinEdge& edge, const JoinGraph& graph, size_t out_rows,
        const Estimate& join_est) const;
};

} // namespace sql::optimizer
