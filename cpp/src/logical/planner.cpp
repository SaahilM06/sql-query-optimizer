#include "planner.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace sql::logical {

namespace {

/// Aggregate function names the planner recognises in the SELECT list.
constexpr std::array<const char*, 5> kAggregateFunctions = {"SUM", "COUNT", "AVG", "MIN", "MAX"};

std::string to_upper(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool is_aggregate_function(const std::string& upper_name) {
    return std::find(kAggregateFunctions.begin(), kAggregateFunctions.end(), upper_name) !=
           kAggregateFunctions.end();
}

} // namespace

void LogicalPlanner::register_alias(const TableRef& table_ref) {
    std::string key = table_ref.alias.value_or(table_ref.table_name);
    alias_map_[key] = table_ref.table_name;
}

std::pair<std::vector<std::pair<Expression, std::optional<std::string>>>, std::vector<AggregateExpr>>
LogicalPlanner::split_select_list(const std::vector<SelectItem>& items) {
    std::vector<std::pair<Expression, std::optional<std::string>>> plain;
    std::vector<AggregateExpr> aggs;

    for (const auto& item : items) {
        // Wildcards are pass-through -- handled at a higher level.
        if (item.kind == SelectItem::Kind::Wildcard || item.kind == SelectItem::Kind::QualifiedWildcard) {
            continue;
        }

        const Expression& expr = *item.expr;
        if (expr.kind == Expression::Kind::Function) {
            std::string upper = to_upper(expr.func_name);
            if (is_aggregate_function(upper)) {
                Expression arg = expr.args.empty() ? Expression::make_wildcard() : expr.args.front();
                aggs.push_back(AggregateExpr{upper, std::move(arg), item.alias});
                continue;
            }
        }
        plain.emplace_back(expr, item.alias);
    }

    return {std::move(plain), std::move(aggs)};
}

LogicalPlan LogicalPlanner::plan(SelectStatement stmt) {
    // 1. Build alias map so later resolution knows "c" = "customers".
    register_alias(stmt.from);
    for (const auto& join : stmt.joins) {
        register_alias(join.table);
    }

    // 2. Scan for the FROM table.
    LogicalPlan plan_node = LogicalPlan::make_scan(stmt.from.table_name, stmt.from.alias);

    // 3. Fold JOINs into a left-deep Join tree.
    //
    //  Scan(a)
    //    |- Join(b)       <- first join
    //         |- Join(c)  <- second join
    //
    for (auto& join : stmt.joins) {
        LogicalPlan right = LogicalPlan::make_scan(join.table.table_name, join.table.alias);
        plan_node = LogicalPlan::make_join(join.join_type, std::move(join.condition),
                                            std::move(plan_node), std::move(right));
    }

    // 4. WHERE -> Filter.
    if (stmt.where_clause.has_value()) {
        plan_node = LogicalPlan::make_filter(std::move(*stmt.where_clause), std::move(plan_node));
    }

    // 5. Split SELECT list into plain columns vs aggregate calls.
    auto [plain_exprs, aggregates] = split_select_list(stmt.columns);
    bool has_aggregates = !aggregates.empty();
    bool has_group_by = !stmt.group_by.empty();

    // 6. GROUP BY + aggregates -> Aggregate.
    if (has_aggregates || has_group_by) {
        plan_node = LogicalPlan::make_aggregate(std::move(stmt.group_by), std::move(aggregates),
                                                 std::move(plan_node));
    }

    // 7. HAVING -> Filter (runs after aggregation).
    if (stmt.having.has_value()) {
        plan_node = LogicalPlan::make_filter(std::move(*stmt.having), std::move(plan_node));
    }

    // 8. ORDER BY -> Sort.
    if (!stmt.order_by.empty()) {
        plan_node = LogicalPlan::make_sort(std::move(stmt.order_by), std::move(plan_node));
    }

    // 9. LIMIT.
    if (stmt.limit.has_value()) {
        plan_node = LogicalPlan::make_limit(*stmt.limit, std::move(plan_node));
    }

    // 10. Project for plain (non-aggregate) SELECT columns.
    //
    // Skip the Project node when:
    //   a) the query is SELECT * (wildcard) -- nothing to project
    //   b) the query uses aggregates -- Aggregate already defines the output
    bool is_wildcard_only = std::all_of(stmt.columns.begin(), stmt.columns.end(), [](const SelectItem& c) {
        return c.kind == SelectItem::Kind::Wildcard || c.kind == SelectItem::Kind::QualifiedWildcard;
    });

    if (!is_wildcard_only && !has_aggregates) {
        plan_node = LogicalPlan::make_project(std::move(plain_exprs), std::move(plan_node));
    }

    return plan_node;
}

} // namespace sql::logical
