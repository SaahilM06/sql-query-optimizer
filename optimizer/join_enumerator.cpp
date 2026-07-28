#include "join_enumerator.hpp"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <stdexcept>

#include "../physical/cost.hpp"

namespace sql::optimizer {

using namespace sql::parser;
using namespace sql::physical;
using sql::logical::Catalog;
using sql::logical::LogicalPlan;
using sql::statistics::StatisticsCatalog;

namespace {

size_t rows_to_size(double rows) { return static_cast<size_t>(std::max(1.0, std::round(rows))); }

int popcount(RelationMask mask) { return static_cast<int>(std::bitset<64>(mask).count()); }

size_t index_of_only_set_bit(RelationMask mask) {
    size_t i = 0;
    while (!((mask >> i) & 1ULL)) ++i;
    return i;
}

// Builds a minimal LogicalPlan standing in for "the relations covered by
// `mask`," purely so CardinalityEstimator's existing resolve_single_table
// machinery (built for ordinary LogicalPlan trees in Part 2) can be reused
// unchanged here. For a single relation this is exactly the real Scan node
// it would have seen outside join search; for anything larger it's a stand-in
// that resolve_single_table is guaranteed to reject, which is exactly the
// "fall back to the default heuristic" behavior we want for a join spanning
// more than one relation on a side.
LogicalPlan synthetic_scope_for_mask(RelationMask mask, const JoinGraph& graph) {
    if (popcount(mask) == 1) {
        const Relation& rel = graph.relations[index_of_only_set_bit(mask)];
        return LogicalPlan::make_scan(rel.table_name, rel.alias);
    }
    // Two dummy scans joined together: not a Scan and not a Filter-over-Scan
    // chain, so resolve_single_table correctly returns nullopt for it.
    LogicalPlan a = LogicalPlan::make_scan("<multi-relation>", std::nullopt);
    LogicalPlan b = LogicalPlan::make_scan("<multi-relation>", std::nullopt);
    return LogicalPlan::make_join(JoinType::Inner, Expression::make_literal(Literal::boolean(true)), std::move(a),
                                   std::move(b));
}

} // namespace

JoinEnumerator::JoinEnumerator(const Catalog& schema_catalog, const StatisticsCatalog& stats_catalog,
                                const CardinalityEstimator& cardinality)
    : schema_catalog_(schema_catalog),
      cardinality_(cardinality),
      access_path_gen_(schema_catalog, stats_catalog, cardinality) {}

bool JoinEnumerator::find_connecting_edge(RelationMask left_mask, RelationMask right_mask, const JoinGraph& graph,
                                           const JoinEdge** out_edge) const {
    for (const auto& edge : graph.edges) {
        RelationMask edge_left_bit = RelationMask(1) << edge.left;
        RelationMask edge_right_bit = RelationMask(1) << edge.right;
        bool connects = ((left_mask & edge_left_bit) && (right_mask & edge_right_bit)) ||
                         ((left_mask & edge_right_bit) && (right_mask & edge_left_bit));
        if (connects) {
            *out_edge = &edge;
            return true;
        }
    }
    return false;
}

std::optional<PhysicalPlan> JoinEnumerator::try_index_nested_loop(RelationMask left_mask, RelationMask right_mask,
                                                                    const PhysicalPlan& left_plan,
                                                                    const PhysicalPlan& right_plan, const JoinEdge& edge,
                                                                    const JoinGraph& graph, size_t out_rows,
                                                                    const Estimate& join_est) const {
    if (edge.predicate.left->kind != Expression::Kind::Column || edge.predicate.right->kind != Expression::Kind::Column) {
        return std::nullopt;
    }

    auto column_for_alias = [&](const std::string& alias) -> const Expression* {
        if (edge.predicate.left->table.has_value() && *edge.predicate.left->table == alias) return edge.predicate.left.get();
        if (edge.predicate.right->table.has_value() && *edge.predicate.right->table == alias) return edge.predicate.right.get();
        return nullptr;
    };

    // Right side is a single base relation with an index on the join key:
    // left is the outer, right is probed.
    if (popcount(right_mask) == 1) {
        const Relation& rel = graph.relations[index_of_only_set_bit(right_mask)];
        const Expression* col = column_for_alias(rel.alias.value_or(rel.table_name));
        const auto* schema = schema_catalog_.get(rel.table_name);
        if (col != nullptr && schema != nullptr && schema->has_index_on(col->column)) {
            cost::Cost c = left_plan.estimated_cost + cost::index_nested_loop_join(left_plan.estimated_rows, out_rows);
            return PhysicalPlan::make_join(PhysicalPlan::Kind::IndexNestedLoopJoin, JoinType::Inner, edge.predicate,
                                            PhysicalPlan(left_plan), PhysicalPlan(right_plan), c, out_rows,
                                            join_est.reasoning, join_est.confidence);
        }
    }

    // Left side is a single base relation with an index on the join key:
    // right is the outer, left is probed.
    if (popcount(left_mask) == 1) {
        const Relation& rel = graph.relations[index_of_only_set_bit(left_mask)];
        const Expression* col = column_for_alias(rel.alias.value_or(rel.table_name));
        const auto* schema = schema_catalog_.get(rel.table_name);
        if (col != nullptr && schema != nullptr && schema->has_index_on(col->column)) {
            cost::Cost c = right_plan.estimated_cost + cost::index_nested_loop_join(right_plan.estimated_rows, out_rows);
            return PhysicalPlan::make_join(PhysicalPlan::Kind::IndexNestedLoopJoin, JoinType::Inner, edge.predicate,
                                            PhysicalPlan(right_plan), PhysicalPlan(left_plan), c, out_rows,
                                            join_est.reasoning, join_est.confidence);
        }
    }

    return std::nullopt;
}

PhysicalPlan JoinEnumerator::generate_best_candidate(RelationMask left_mask, RelationMask right_mask,
                                                      const PhysicalPlan& left_plan, const PhysicalPlan& right_plan,
                                                      const JoinEdge& edge, const JoinGraph& graph) const {
    LogicalPlan left_scope = synthetic_scope_for_mask(left_mask, graph);
    LogicalPlan right_scope = synthetic_scope_for_mask(right_mask, graph);

    Estimate join_est = cardinality_.estimate_join(static_cast<double>(left_plan.estimated_rows),
                                                    static_cast<double>(right_plan.estimated_rows), edge.predicate,
                                                    left_scope, right_scope);
    size_t out_rows = rows_to_size(join_est.rows);

    bool is_equi = edge.predicate.kind == Expression::Kind::BinaryOp && edge.predicate.binary_op == BinaryOperator::Eq;

    std::vector<PhysicalPlan> candidates;

    // NestedLoopJoin, both orientations -- genuinely different cost, since
    // whichever side is "outer" gets rescanned via the other's full cost
    // once per outer row.
    {
        cost::Cost c = left_plan.estimated_cost;
        c.cpu += static_cast<double>(left_plan.estimated_rows) * right_plan.estimated_cost.total();
        c += cost::nested_loop_join(left_plan.estimated_rows, right_plan.estimated_rows);
        candidates.push_back(PhysicalPlan::make_join(PhysicalPlan::Kind::NestedLoopJoin, JoinType::Inner, edge.predicate,
                                                       PhysicalPlan(left_plan), PhysicalPlan(right_plan), c, out_rows,
                                                       join_est.reasoning, join_est.confidence));
    }
    {
        cost::Cost c = right_plan.estimated_cost;
        c.cpu += static_cast<double>(right_plan.estimated_rows) * left_plan.estimated_cost.total();
        c += cost::nested_loop_join(right_plan.estimated_rows, left_plan.estimated_rows);
        candidates.push_back(PhysicalPlan::make_join(PhysicalPlan::Kind::NestedLoopJoin, JoinType::Inner, edge.predicate,
                                                       PhysicalPlan(right_plan), PhysicalPlan(left_plan), c, out_rows,
                                                       join_est.reasoning, join_est.confidence));
    }

    if (is_equi) {
        cost::Cost c = left_plan.estimated_cost + right_plan.estimated_cost +
                        cost::hash_join(left_plan.estimated_rows, right_plan.estimated_rows);
        candidates.push_back(PhysicalPlan::make_join(PhysicalPlan::Kind::HashJoin, JoinType::Inner, edge.predicate,
                                                       PhysicalPlan(left_plan), PhysicalPlan(right_plan), c, out_rows,
                                                       join_est.reasoning, join_est.confidence));

        if (auto idx = try_index_nested_loop(left_mask, right_mask, left_plan, right_plan, edge, graph, out_rows,
                                              join_est)) {
            candidates.push_back(std::move(*idx));
        }
    }

    size_t best_i = 0;
    for (size_t i = 1; i < candidates.size(); ++i) {
        if (candidates[i].estimated_cost.total() < candidates[best_i].estimated_cost.total()) best_i = i;
    }
    return std::move(candidates[best_i]);
}

void JoinEnumerator::solve_subset(RelationMask subset, const JoinGraph& graph) {
    std::optional<PhysicalPlan> best;

    for (size_t r = 0; r < graph.relations.size(); ++r) {
        RelationMask r_mask = RelationMask(1) << r;
        if (!(subset & r_mask)) continue;

        RelationMask left_mask = subset & ~r_mask;
        if (left_mask == 0) continue; // subset is just {r} -- that's the size-1 base case, not a join

        auto left_it = memo_.find(left_mask);
        if (left_it == memo_.end()) continue; // left_mask itself isn't a connected, solvable subset

        auto right_it = memo_.find(r_mask);
        if (right_it == memo_.end()) continue; // shouldn't happen -- base cases are always populated first

        const JoinEdge* edge = nullptr;
        if (!find_connecting_edge(left_mask, r_mask, graph, &edge)) continue; // no edge -- refuse the cartesian product

        PhysicalPlan candidate =
            generate_best_candidate(left_mask, r_mask, left_it->second, right_it->second, *edge, graph);
        if (!best.has_value() || candidate.estimated_cost.total() < best->estimated_cost.total()) {
            best = std::move(candidate);
        }
    }

    if (best.has_value()) {
        memo_[subset] = std::move(*best);
    }
}

PhysicalPlan JoinEnumerator::find_best_plan(const JoinGraph& graph) {
    size_t n = graph.relations.size();
    if (n == 0) throw std::logic_error("join_enumerator: empty join graph");

    for (size_t i = 0; i < n; ++i) {
        RelationMask mask = RelationMask(1) << i;
        memo_[mask] = access_path_gen_.best_access_path(graph.relations[i]);
    }

    if (n == 1) return std::move(memo_[RelationMask(1)]);

    for (int size = 2; size <= static_cast<int>(n); ++size) {
        RelationMask limit = (n == 64) ? ~RelationMask(0) : (RelationMask(1) << n);
        for (RelationMask subset = 1; subset < limit; ++subset) {
            if (popcount(subset) != size) continue;
            solve_subset(subset, graph);
        }
    }

    RelationMask full = (n == 64) ? ~RelationMask(0) : ((RelationMask(1) << n) - 1);
    auto it = memo_.find(full);
    if (it == memo_.end()) {
        throw std::runtime_error(
            "join_enumerator: relations are not fully connected by join predicates -- "
            "a cartesian product would be required, which this search refuses to generate");
    }
    return std::move(it->second);
}

} // namespace sql::optimizer
