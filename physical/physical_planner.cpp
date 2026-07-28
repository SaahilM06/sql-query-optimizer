#include "physical_planner.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "../optimizer/join_enumerator.hpp"
#include "../optimizer/join_graph.hpp"
#include "cost.hpp"

namespace sql::physical {

using sql::optimizer::Estimate;
using sql::parser::BinaryOperator;

namespace {

size_t rows_to_size(double rows) {
    return static_cast<size_t>(std::max(1.0, std::round(rows)));
}

} // namespace

PhysicalPlan generate_physical_plan(const LogicalPlan& logical, const Catalog& schema_catalog,
                                     const StatisticsCatalog& stats_catalog) {
    PhysicalPlanner planner(schema_catalog, stats_catalog);
    return planner.plan(logical);
}

PhysicalPlan PhysicalPlanner::plan(const LogicalPlan& logical) {
    return plan_node(logical);
}

PhysicalPlan PhysicalPlanner::plan_scan(const LogicalPlan& scan) {
    Estimate est = cardinality_.estimate_scan(scan.table_name);
    size_t row_count = rows_to_size(est.rows);

    const auto* stats = stats_catalog_.get(scan.table_name);
    double page_count = (stats != nullptr && stats->page_count > 0.0) ? stats->page_count
                                                                       : cost::pages_for(row_count, 64);

    cost::Cost c = cost::seq_scan(row_count, page_count);
    return PhysicalPlan::make_seq_scan(scan.table_name, scan.alias, scan.projected_columns, c, row_count,
                                        est.reasoning, est.confidence);
}

// Recognizes `Filter(Column = Literal) over Scan(indexed column)` -- the
// shape predicate pushdown leaves single-table equality filters in -- and
// collapses it into a single IndexScan when that's cheaper than SeqScan +
// Filter. Returns nullopt if the pattern doesn't match or SeqScan wins,
// letting the caller fall back to the normal Filter-over-Scan path.
//
// Both candidates' row estimates come from the same CardinalityEstimator
// call, so the comparison is apples-to-apples -- only the access-method
// cost formula differs between them.
std::optional<PhysicalPlan> PhysicalPlanner::try_index_scan(const LogicalPlan& filter_node,
                                                             const LogicalPlan& scan_node) {
    const Expression& pred = filter_node.predicate;
    if (pred.kind != Expression::Kind::BinaryOp || pred.binary_op != BinaryOperator::Eq) {
        return std::nullopt;
    }

    const Expression* col_side = nullptr;
    const Expression* lit_side = nullptr;
    if (pred.left->kind == Expression::Kind::Column && pred.right->kind == Expression::Kind::Literal) {
        col_side = pred.left.get();
        lit_side = pred.right.get();
    } else if (pred.right->kind == Expression::Kind::Column && pred.left->kind == Expression::Kind::Literal) {
        col_side = pred.right.get();
        lit_side = pred.left.get();
    } else {
        return std::nullopt;
    }

    std::string scan_ident = scan_node.alias.value_or(scan_node.table_name);
    if (col_side->table.has_value() && *col_side->table != scan_ident) {
        return std::nullopt; // references a different relation
    }

    const auto* schema = schema_catalog_.get(scan_node.table_name);
    if (schema == nullptr || !schema->has_index_on(col_side->column)) {
        return std::nullopt;
    }

    Estimate scan_est = cardinality_.estimate_scan(scan_node.table_name);
    size_t row_count = rows_to_size(scan_est.rows);

    Estimate filter_est = cardinality_.estimate_filter(scan_est.rows, pred, scan_node);
    size_t matched_rows = rows_to_size(filter_est.rows);

    const auto* stats = stats_catalog_.get(scan_node.table_name);
    double page_count = (stats != nullptr && stats->page_count > 0.0) ? stats->page_count
                                                                       : cost::pages_for(row_count, 64);

    cost::Cost index_cost = cost::index_scan(matched_rows);
    cost::Cost seq_plus_filter_cost = cost::seq_scan(row_count, page_count) + cost::filter(row_count);

    if (index_cost.total() >= seq_plus_filter_cost.total()) {
        return std::nullopt; // SeqScan + Filter wins -- not selective enough to justify the index
    }

    return PhysicalPlan::make_index_scan(scan_node.table_name, scan_node.alias, scan_node.projected_columns,
                                          col_side->column, *lit_side, index_cost, matched_rows, filter_est.reasoning,
                                          filter_est.confidence);
}

PhysicalPlan PhysicalPlanner::choose_join_strategy(JoinType join_type, Expression condition, PhysicalPlan left,
                                                    PhysicalPlan right, const LogicalPlan& left_scope,
                                                    const LogicalPlan& right_scope) {
    Estimate join_est = cardinality_.estimate_join(static_cast<double>(left.estimated_rows),
                                                    static_cast<double>(right.estimated_rows), condition, left_scope,
                                                    right_scope);
    size_t out_rows = rows_to_size(join_est.rows);

    // Nested loop join re-invokes the right subplan once per left row --
    // that's why left.estimated_cost only appears once but right.estimated_cost
    // is scaled by left.estimated_rows.
    cost::Cost nl_cost = left.estimated_cost;
    nl_cost.cpu += static_cast<double>(left.estimated_rows) * right.estimated_cost.total();
    nl_cost += cost::nested_loop_join(left.estimated_rows, right.estimated_rows);

    bool is_equi_join = condition.kind == Expression::Kind::BinaryOp && condition.binary_op == BinaryOperator::Eq;

    if (!is_equi_join) {
        return PhysicalPlan::make_join(PhysicalPlan::Kind::NestedLoopJoin, join_type, std::move(condition),
                                        std::move(left), std::move(right), nl_cost, out_rows, join_est.reasoning,
                                        join_est.confidence);
    }

    // Hash join reads each side exactly once (build on the smaller side,
    // probe with the larger), so child costs are added, not multiplied.
    cost::Cost hj_cost = left.estimated_cost + right.estimated_cost +
                          cost::hash_join(left.estimated_rows, right.estimated_rows);

    if (hj_cost.total() < nl_cost.total()) {
        return PhysicalPlan::make_join(PhysicalPlan::Kind::HashJoin, join_type, std::move(condition), std::move(left),
                                        std::move(right), hj_cost, out_rows, join_est.reasoning, join_est.confidence);
    }
    return PhysicalPlan::make_join(PhysicalPlan::Kind::NestedLoopJoin, join_type, std::move(condition),
                                    std::move(left), std::move(right), nl_cost, out_rows, join_est.reasoning,
                                    join_est.confidence);
}

// Extracts a join graph from a maximal all-INNER-join subtree (Part 3) and
// hands it to JoinEnumerator, which searches join order and physical join
// algorithm together. Any predicate the graph builder couldn't attribute
// to exactly one or two relations (residual_filters) is applied as an
// ordinary Filter wrapping the search's result, using the same default
// heuristic every other unresolvable-scope filter in this planner falls
// back to.
PhysicalPlan PhysicalPlanner::plan_join_search(const LogicalPlan& node) {
    sql::optimizer::JoinGraphExtraction extraction = sql::optimizer::build_join_graph(node);

    sql::optimizer::JoinEnumerator enumerator(schema_catalog_, stats_catalog_, cardinality_);
    PhysicalPlan best = enumerator.find_best_plan(extraction.graph);

    for (auto& pred : extraction.residual_filters) {
        size_t rows = std::max<size_t>(best.estimated_rows / 10, static_cast<size_t>(1));
        cost::Cost c = best.estimated_cost + cost::filter(best.estimated_rows);
        best = PhysicalPlan::make_filter(
            pred, std::move(best), c, rows,
            "residual predicate spanning more than two relations or an unresolvable column -- default 10% heuristic",
            0.2);
    }

    return best;
}

PhysicalPlan PhysicalPlanner::plan_node(const LogicalPlan& node) {
    if (sql::optimizer::is_join_search_candidate(node)) {
        return plan_join_search(node);
    }

    switch (node.kind) {
        case LogicalPlan::Kind::Scan:
            return plan_scan(node);

        case LogicalPlan::Kind::Filter: {
            if (node.input->kind == LogicalPlan::Kind::Scan) {
                if (auto idx = try_index_scan(node, *node.input)) {
                    return std::move(*idx);
                }
            }
            PhysicalPlan input = plan_node(*node.input);
            Estimate est = cardinality_.estimate_filter(static_cast<double>(input.estimated_rows), node.predicate,
                                                          *node.input);
            size_t rows = rows_to_size(est.rows);
            cost::Cost c = input.estimated_cost + cost::filter(input.estimated_rows);
            return PhysicalPlan::make_filter(node.predicate, std::move(input), c, rows, est.reasoning, est.confidence);
        }

        case LogicalPlan::Kind::Join: {
            PhysicalPlan left = plan_node(*node.left);
            PhysicalPlan right = plan_node(*node.right);
            return choose_join_strategy(node.join_type, node.condition, std::move(left), std::move(right),
                                         *node.left, *node.right);
        }

        case LogicalPlan::Kind::Aggregate: {
            PhysicalPlan input = plan_node(*node.input);
            Estimate est = cardinality_.estimate_aggregate(static_cast<double>(input.estimated_rows), node.group_by,
                                                             *node.input);
            size_t rows = rows_to_size(est.rows);
            cost::Cost c = input.estimated_cost + cost::hash_aggregate(input.estimated_rows, rows);
            return PhysicalPlan::make_hash_aggregate(node.group_by, node.aggregates, std::move(input), c, rows,
                                                       est.reasoning, est.confidence);
        }

        case LogicalPlan::Kind::Project: {
            PhysicalPlan input = plan_node(*node.input);
            Estimate est = CardinalityEstimator::estimate_project(static_cast<double>(input.estimated_rows));
            size_t rows = rows_to_size(est.rows);
            cost::Cost c = input.estimated_cost + cost::project(input.estimated_rows);
            return PhysicalPlan::make_project(node.expressions, std::move(input), c, rows, est.reasoning,
                                               est.confidence);
        }

        case LogicalPlan::Kind::Sort: {
            PhysicalPlan input = plan_node(*node.input);
            Estimate est = CardinalityEstimator::estimate_sort(static_cast<double>(input.estimated_rows));
            size_t rows = rows_to_size(est.rows);
            cost::Cost c = input.estimated_cost + cost::sort(input.estimated_rows);
            return PhysicalPlan::make_sort(node.order_by, std::move(input), c, rows, est.reasoning, est.confidence);
        }

        case LogicalPlan::Kind::Limit: {
            PhysicalPlan input = plan_node(*node.input);
            Estimate est = CardinalityEstimator::estimate_limit(static_cast<double>(input.estimated_rows), node.count);
            size_t rows = rows_to_size(est.rows);
            cost::Cost c = input.estimated_cost + cost::limit(input.estimated_rows);
            return PhysicalPlan::make_limit(node.count, std::move(input), c, rows, est.reasoning, est.confidence);
        }
    }
    throw std::logic_error("unreachable: unknown LogicalPlan::Kind");
}

} // namespace sql::physical
