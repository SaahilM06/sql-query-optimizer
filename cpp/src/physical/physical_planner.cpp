#include "physical_planner.hpp"

#include <algorithm>
#include <stdexcept>

#include "cost.hpp"

namespace sql::physical {

using sql::parser::BinaryOperator;

PhysicalPlan generate_physical_plan(const LogicalPlan& logical, const Catalog& catalog) {
    PhysicalPlanner planner(catalog);
    return planner.plan(logical);
}

PhysicalPlan PhysicalPlanner::plan(const LogicalPlan& logical) {
    return plan_node(logical);
}

PhysicalPlan PhysicalPlanner::plan_scan(const LogicalPlan& scan) {
    const auto* schema = catalog_.get(scan.table_name);
    size_t row_count = schema ? schema->stats.row_count : 1000;
    size_t avg_row_bytes = schema ? schema->stats.avg_row_bytes : 64;

    double c = cost::seq_scan(row_count, avg_row_bytes);
    return PhysicalPlan::make_seq_scan(scan.table_name, scan.alias, scan.projected_columns, c, row_count);
}

// Recognizes `Filter(Column = Literal) over Scan(indexed column)` -- the
// shape predicate pushdown leaves single-table equality filters in -- and
// collapses it into a single IndexScan when that's cheaper than SeqScan +
// Filter. Returns nullopt if the pattern doesn't match or SeqScan wins,
// letting the caller fall back to the normal Filter-over-Scan path.
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

    const auto* schema = catalog_.get(scan_node.table_name);
    if (!schema || !schema->has_index_on(col_side->column)) {
        return std::nullopt;
    }

    size_t row_count = schema->stats.row_count;
    size_t avg_row_bytes = schema->stats.avg_row_bytes;

    // Placeholder selectivity: assume an equality predicate matches ~0.1% of
    // rows (a specific-value lookup). Replaced with real per-column
    // distinct-value statistics in the cardinality-estimation phase.
    size_t matched_rows = std::max<size_t>(row_count / 1000, static_cast<size_t>(1));

    double index_cost = cost::index_scan(matched_rows, avg_row_bytes);
    double seq_plus_filter_cost = cost::seq_scan(row_count, avg_row_bytes) + cost::filter(row_count);

    if (index_cost >= seq_plus_filter_cost) {
        return std::nullopt; // SeqScan + Filter wins -- not selective enough to justify the index
    }

    return PhysicalPlan::make_index_scan(scan_node.table_name, scan_node.alias, scan_node.projected_columns,
                                          col_side->column, *lit_side, index_cost, matched_rows);
}

PhysicalPlan PhysicalPlanner::choose_join_strategy(JoinType join_type, Expression condition,
                                                    PhysicalPlan left, PhysicalPlan right) {
    bool is_equi_join = condition.kind == Expression::Kind::BinaryOp && condition.binary_op == BinaryOperator::Eq;
    size_t out_rows = estimate_join_rows(left.estimated_rows, right.estimated_rows);

    // Nested loop join re-invokes the right subplan once per left row --
    // that's why left.estimated_cost only appears once but right.estimated_cost
    // is scaled by left.estimated_rows.
    double nl_cost = left.estimated_cost + static_cast<double>(left.estimated_rows) * right.estimated_cost +
                      cost::nested_loop_join(left.estimated_rows, right.estimated_rows);

    if (!is_equi_join) {
        return PhysicalPlan::make_join(PhysicalPlan::Kind::NestedLoopJoin, join_type, std::move(condition),
                                        std::move(left), std::move(right), nl_cost, out_rows);
    }

    // Hash join reads each side exactly once (build on the smaller side,
    // probe with the larger), so child costs are added, not multiplied.
    double hj_cost = left.estimated_cost + right.estimated_cost +
                      cost::hash_join(left.estimated_rows, right.estimated_rows);

    if (hj_cost < nl_cost) {
        return PhysicalPlan::make_join(PhysicalPlan::Kind::HashJoin, join_type, std::move(condition),
                                        std::move(left), std::move(right), hj_cost, out_rows);
    }
    return PhysicalPlan::make_join(PhysicalPlan::Kind::NestedLoopJoin, join_type, std::move(condition),
                                    std::move(left), std::move(right), nl_cost, out_rows);
}

size_t PhysicalPlanner::estimate_filter_rows(size_t input_rows) const {
    // Same flat heuristic the logical optimizer uses today; both get
    // replaced by real selectivity in the cardinality-estimation phase.
    return std::max<size_t>(input_rows / 10, static_cast<size_t>(1));
}

size_t PhysicalPlanner::estimate_join_rows(size_t left_rows, size_t right_rows) const {
    return std::max<size_t>((left_rows * right_rows) / 10, static_cast<size_t>(1));
}

PhysicalPlan PhysicalPlanner::plan_node(const LogicalPlan& node) {
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
            double c = input.estimated_cost + cost::filter(input.estimated_rows);
            size_t rows = estimate_filter_rows(input.estimated_rows);
            return PhysicalPlan::make_filter(node.predicate, std::move(input), c, rows);
        }

        case LogicalPlan::Kind::Join: {
            PhysicalPlan left = plan_node(*node.left);
            PhysicalPlan right = plan_node(*node.right);
            return choose_join_strategy(node.join_type, node.condition, std::move(left), std::move(right));
        }

        case LogicalPlan::Kind::Aggregate: {
            PhysicalPlan input = plan_node(*node.input);
            double c = input.estimated_cost + cost::hash_aggregate(input.estimated_rows);
            size_t rows = std::max<size_t>(input.estimated_rows / 10, static_cast<size_t>(1));
            return PhysicalPlan::make_hash_aggregate(node.group_by, node.aggregates, std::move(input), c, rows);
        }

        case LogicalPlan::Kind::Project: {
            PhysicalPlan input = plan_node(*node.input);
            double c = input.estimated_cost + cost::project(input.estimated_rows);
            size_t rows = input.estimated_rows;
            return PhysicalPlan::make_project(node.expressions, std::move(input), c, rows);
        }

        case LogicalPlan::Kind::Sort: {
            PhysicalPlan input = plan_node(*node.input);
            double c = input.estimated_cost + cost::sort(input.estimated_rows);
            size_t rows = input.estimated_rows;
            return PhysicalPlan::make_sort(node.order_by, std::move(input), c, rows);
        }

        case LogicalPlan::Kind::Limit: {
            PhysicalPlan input = plan_node(*node.input);
            double c = input.estimated_cost + cost::limit(input.estimated_rows);
            size_t rows = std::min(input.estimated_rows, node.count);
            return PhysicalPlan::make_limit(node.count, std::move(input), c, rows);
        }
    }
    throw std::logic_error("unreachable: unknown LogicalPlan::Kind");
}

} // namespace sql::physical
