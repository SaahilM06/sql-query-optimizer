#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../parser/ast.hpp"

namespace sql::logical {

using sql::parser::Expression;
using sql::parser::JoinType;
using sql::parser::OrderByItem;

// ── Aggregate helper ─────────────────────────────────────────────────────────
//
// One aggregate call extracted from the SELECT list.
// e.g. `SUM(o.total) AS total` -> func="SUM", arg=Column(o.total), alias=Some("total")
struct AggregateExpr {
    std::string func;              // "SUM" | "COUNT" | "AVG" | "MIN" | "MAX"
    Expression arg;                // the argument -- Expression::Wildcard for COUNT(*)
    std::optional<std::string> alias;
};

// ── Logical plan tree ────────────────────────────────────────────────────────
//
// Mirrors the Rust `LogicalPlan` enum. Move-only: nothing in the optimizer
// pipeline ever deep-clones a LogicalPlan node, only rebuilds trees by moving
// children out (matching Rust's `match plan { .. }` moving out of `Box<T>`).
class LogicalPlan {
public:
    enum class Kind { Scan, Filter, Join, Aggregate, Project, Sort, Limit };

    Kind kind = Kind::Scan;

    // Scan { table_name, alias, projected_columns }
    // `projected_columns` is set by projection pushdown -- empty means all columns.
    std::string table_name;
    std::optional<std::string> alias;
    std::vector<std::string> projected_columns;

    // Filter { predicate, input } / shared `input` slot for all single-child nodes
    Expression predicate;
    std::unique_ptr<LogicalPlan> input;

    // Join { join_type, condition, left, right }
    JoinType join_type = JoinType::Inner;
    Expression condition;
    std::unique_ptr<LogicalPlan> left;
    std::unique_ptr<LogicalPlan> right;

    // Aggregate { group_by, aggregates, input }
    std::vector<Expression> group_by;
    std::vector<AggregateExpr> aggregates;

    // Project { expressions, input }
    std::vector<std::pair<Expression, std::optional<std::string>>> expressions;

    // Sort { order_by, input }
    std::vector<OrderByItem> order_by;

    // Limit { count, input }
    size_t count = 0;

    LogicalPlan() = default;
    LogicalPlan(const LogicalPlan&) = delete;
    LogicalPlan& operator=(const LogicalPlan&) = delete;
    LogicalPlan(LogicalPlan&&) noexcept = default;
    LogicalPlan& operator=(LogicalPlan&&) noexcept = default;

    static LogicalPlan make_scan(std::string table_name, std::optional<std::string> alias,
                                  std::vector<std::string> projected_columns = {}) {
        LogicalPlan p;
        p.kind = Kind::Scan;
        p.table_name = std::move(table_name);
        p.alias = std::move(alias);
        p.projected_columns = std::move(projected_columns);
        return p;
    }

    static LogicalPlan make_filter(Expression predicate, LogicalPlan input) {
        LogicalPlan p;
        p.kind = Kind::Filter;
        p.predicate = std::move(predicate);
        p.input = std::make_unique<LogicalPlan>(std::move(input));
        return p;
    }

    static LogicalPlan make_join(JoinType join_type, Expression condition,
                                  LogicalPlan left, LogicalPlan right) {
        LogicalPlan p;
        p.kind = Kind::Join;
        p.join_type = join_type;
        p.condition = std::move(condition);
        p.left = std::make_unique<LogicalPlan>(std::move(left));
        p.right = std::make_unique<LogicalPlan>(std::move(right));
        return p;
    }

    static LogicalPlan make_aggregate(std::vector<Expression> group_by,
                                       std::vector<AggregateExpr> aggregates,
                                       LogicalPlan input) {
        LogicalPlan p;
        p.kind = Kind::Aggregate;
        p.group_by = std::move(group_by);
        p.aggregates = std::move(aggregates);
        p.input = std::make_unique<LogicalPlan>(std::move(input));
        return p;
    }

    static LogicalPlan make_project(std::vector<std::pair<Expression, std::optional<std::string>>> expressions,
                                     LogicalPlan input) {
        LogicalPlan p;
        p.kind = Kind::Project;
        p.expressions = std::move(expressions);
        p.input = std::make_unique<LogicalPlan>(std::move(input));
        return p;
    }

    static LogicalPlan make_sort(std::vector<OrderByItem> order_by, LogicalPlan input) {
        LogicalPlan p;
        p.kind = Kind::Sort;
        p.order_by = std::move(order_by);
        p.input = std::make_unique<LogicalPlan>(std::move(input));
        return p;
    }

    static LogicalPlan make_limit(size_t count, LogicalPlan input) {
        LogicalPlan p;
        p.kind = Kind::Limit;
        p.count = count;
        p.input = std::make_unique<LogicalPlan>(std::move(input));
        return p;
    }
};

} // namespace sql::logical
