#include "cardinality_estimator.hpp"

#include <algorithm>
#include <sstream>

namespace sql::optimizer {

using namespace sql::parser;
using namespace sql::logical;
using namespace sql::statistics;

std::optional<SingleTableScope> resolve_single_table(const LogicalPlan& scope) {
    switch (scope.kind) {
        case LogicalPlan::Kind::Scan:
            return SingleTableScope{scope.table_name, scope.alias.value_or(scope.table_name)};
        case LogicalPlan::Kind::Filter:
            return resolve_single_table(*scope.input);
        default:
            return std::nullopt; // Join, Aggregate, Project, Sort, Limit -- not a single table
    }
}

Estimate CardinalityEstimator::estimate_scan(const std::string& table_name) const {
    const TableStats* stats = catalog_.get(table_name);
    if (stats == nullptr) {
        return Estimate{1000.0, 0.2, "no statistics for table '" + table_name + "' -- default row-count guess"};
    }
    return Estimate{stats->row_count, 1.0, "base row count from statistics catalog"};
}

Estimate CardinalityEstimator::estimate_filter(double input_rows, const Expression& predicate,
                                                const LogicalPlan& scope) const {
    auto single = resolve_single_table(scope);
    if (!single.has_value()) {
        double rows = std::max(1.0, input_rows / 10.0);
        return Estimate{rows, 0.2, "predicate scope spans multiple relations -- default 10% heuristic"};
    }

    const TableStats* stats = catalog_.get(single->table_name);
    if (stats == nullptr) {
        double rows = std::max(1.0, input_rows / 10.0);
        return Estimate{rows, 0.2, "no statistics for table '" + single->table_name + "' -- default 10% heuristic"};
    }

    double sel = selectivity_.estimate(predicate, *stats);
    double rows = std::max(1.0, input_rows * sel);

    std::ostringstream reasoning;
    reasoning << "selectivity " << sel << " estimated from statistics for table '" << single->table_name << "'";
    return Estimate{rows, 0.8, reasoning.str()};
}

Estimate CardinalityEstimator::estimate_join(double left_rows, double right_rows, const Expression& condition,
                                              const LogicalPlan& left_scope, const LogicalPlan& right_scope) const {
    double fallback_rows = std::max(1.0, (left_rows * right_rows) / 10.0);
    Estimate fallback{fallback_rows, 0.2, "default 10%-of-cartesian-product heuristic (non-equi condition or statistics unavailable)"};

    if (condition.kind != Expression::Kind::BinaryOp || condition.binary_op != BinaryOperator::Eq) {
        return fallback;
    }
    if (condition.left->kind != Expression::Kind::Column || condition.right->kind != Expression::Kind::Column) {
        return fallback;
    }

    auto left_single = resolve_single_table(left_scope);
    auto right_single = resolve_single_table(right_scope);
    if (!left_single.has_value() || !right_single.has_value()) return fallback;

    auto matches = [](const std::optional<std::string>& qualifier, const std::string& alias_or_name) {
        return !qualifier.has_value() || *qualifier == alias_or_name;
    };

    const Expression* left_col_expr = nullptr;
    const Expression* right_col_expr = nullptr;

    if (matches(condition.left->table, left_single->alias_or_name) && matches(condition.right->table, right_single->alias_or_name)) {
        left_col_expr = condition.left.get();
        right_col_expr = condition.right.get();
    } else if (matches(condition.right->table, left_single->alias_or_name) &&
               matches(condition.left->table, right_single->alias_or_name)) {
        left_col_expr = condition.right.get();
        right_col_expr = condition.left.get();
    } else {
        return fallback; // condition doesn't relate these two specific relations
    }

    const TableStats* left_stats = catalog_.get(left_single->table_name);
    const TableStats* right_stats = catalog_.get(right_single->table_name);
    if (left_stats == nullptr || right_stats == nullptr) return fallback;

    const ColumnStats* left_col_stats = left_stats->get_column(left_col_expr->column);
    const ColumnStats* right_col_stats = right_stats->get_column(right_col_expr->column);
    if (left_col_stats == nullptr || right_col_stats == nullptr) return fallback;

    double left_ndv = left_col_stats->distinct_count;
    double right_ndv = right_col_stats->distinct_count;
    if (left_ndv <= 0.0 || right_ndv <= 0.0) return fallback;

    double denom = std::max(left_ndv, right_ndv);
    double rows = std::max(1.0, (left_rows * right_rows) / denom);

    std::ostringstream reasoning;
    reasoning << "equi-join NDV formula: (" << left_rows << " x " << right_rows << ") / max(" << left_ndv << ", "
              << right_ndv << ") on " << left_single->table_name << "." << left_col_expr->column << " = "
              << right_single->table_name << "." << right_col_expr->column;
    return Estimate{rows, 0.7, reasoning.str()};
}

Estimate CardinalityEstimator::estimate_aggregate(double input_rows, const std::vector<Expression>& group_by,
                                                   const LogicalPlan& scope) const {
    if (group_by.empty()) {
        return Estimate{1.0, 0.9, "no GROUP BY -- a single aggregate row"};
    }

    auto single = resolve_single_table(scope);
    if (!single.has_value()) {
        double rows = std::max(1.0, input_rows / 10.0);
        return Estimate{rows, 0.2, "GROUP BY over a multi-relation scope -- default 10% heuristic"};
    }

    const TableStats* stats = catalog_.get(single->table_name);
    if (stats == nullptr) {
        double rows = std::max(1.0, input_rows / 10.0);
        return Estimate{rows, 0.2, "no statistics for table '" + single->table_name + "' -- default 10% heuristic"};
    }

    double groups = 1.0;
    for (const auto& expr : group_by) {
        if (expr.kind != Expression::Kind::Column) {
            double rows = std::max(1.0, input_rows / 10.0);
            return Estimate{rows, 0.2, "non-column GROUP BY expression -- default 10% heuristic"};
        }
        const ColumnStats* cs = stats->get_column(expr.column);
        if (cs == nullptr || cs->distinct_count <= 0.0) {
            double rows = std::max(1.0, input_rows / 10.0);
            return Estimate{rows, 0.2, "no NDV statistic for GROUP BY column '" + expr.column + "' -- default 10% heuristic"};
        }
        groups *= cs->distinct_count;
    }

    double rows = std::max(1.0, std::min(input_rows, groups));
    std::ostringstream reasoning;
    reasoning << "product of GROUP BY column NDVs (" << groups << "), capped at input row count (" << input_rows << ")";
    return Estimate{rows, 0.6, reasoning.str()};
}

Estimate CardinalityEstimator::estimate_project(double input_rows) {
    return Estimate{input_rows, 1.0, "projection does not change row count"};
}

Estimate CardinalityEstimator::estimate_sort(double input_rows) {
    return Estimate{input_rows, 1.0, "sort does not change row count"};
}

Estimate CardinalityEstimator::estimate_limit(double input_rows, size_t count) {
    double rows = std::min(input_rows, static_cast<double>(count));
    return Estimate{rows, 1.0, "min(input rows, LIMIT " + std::to_string(count) + ")"};
}

} // namespace sql::optimizer
