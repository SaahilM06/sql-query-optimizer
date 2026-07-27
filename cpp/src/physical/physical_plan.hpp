#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../logical/logical_plan.hpp"
#include "../parser/ast.hpp"

namespace sql::physical {

using sql::logical::AggregateExpr;
using sql::parser::Expression;
using sql::parser::JoinType;
using sql::parser::OrderByItem;

// ── Physical plan tree ───────────────────────────────────────────────────────
//
// One concrete execution strategy per logical node. Where the logical plan
// only says "join these two inputs," the physical plan says exactly how:
// NestedLoopJoin or HashJoin, SeqScan or IndexScan. Each node carries the
// cumulative estimated cost and row count for the subtree rooted at it (not
// just its own local cost) -- that's what makes "pick the cheaper candidate"
// a meaningful comparison at every level, not just the top.
//
// Move-only, same rationale as LogicalPlan: nothing here needs deep cloning,
// only tree rebuilding by moving children out.
class PhysicalPlan {
public:
    enum class Kind {
        SeqScan,
        IndexScan,
        Filter,
        NestedLoopJoin,
        HashJoin,
        HashAggregate,
        Project,
        Sort,
        Limit,
    };

    Kind kind = Kind::SeqScan;
    double estimated_cost = 0.0; // cumulative cost of this subtree
    size_t estimated_rows = 0;   // cumulative output row estimate

    // SeqScan / IndexScan
    std::string table_name;
    std::optional<std::string> alias;
    std::vector<std::string> projected_columns;
    // IndexScan only -- the equality lookup this index scan applies
    // (folded in from what would otherwise be a separate Filter node).
    std::string index_column;
    Expression index_probe_value;

    // Filter
    Expression predicate;
    std::unique_ptr<PhysicalPlan> input; // shared single-child slot

    // NestedLoopJoin / HashJoin
    JoinType join_type = JoinType::Inner;
    Expression condition;
    std::unique_ptr<PhysicalPlan> left;
    std::unique_ptr<PhysicalPlan> right;

    // HashAggregate
    std::vector<Expression> group_by;
    std::vector<AggregateExpr> aggregates;

    // Project
    std::vector<std::pair<Expression, std::optional<std::string>>> expressions;

    // Sort
    std::vector<OrderByItem> order_by;

    // Limit
    size_t count = 0;

    PhysicalPlan() = default;
    PhysicalPlan(const PhysicalPlan&) = delete;
    PhysicalPlan& operator=(const PhysicalPlan&) = delete;
    PhysicalPlan(PhysicalPlan&&) noexcept = default;
    PhysicalPlan& operator=(PhysicalPlan&&) noexcept = default;

    static PhysicalPlan make_seq_scan(std::string table_name, std::optional<std::string> alias,
                                       std::vector<std::string> projected_columns,
                                       double cost, size_t rows) {
        PhysicalPlan p;
        p.kind = Kind::SeqScan;
        p.table_name = std::move(table_name);
        p.alias = std::move(alias);
        p.projected_columns = std::move(projected_columns);
        p.estimated_cost = cost;
        p.estimated_rows = rows;
        return p;
    }

    static PhysicalPlan make_index_scan(std::string table_name, std::optional<std::string> alias,
                                         std::vector<std::string> projected_columns,
                                         std::string index_column, Expression probe_value,
                                         double cost, size_t rows) {
        PhysicalPlan p;
        p.kind = Kind::IndexScan;
        p.table_name = std::move(table_name);
        p.alias = std::move(alias);
        p.projected_columns = std::move(projected_columns);
        p.index_column = std::move(index_column);
        p.index_probe_value = std::move(probe_value);
        p.estimated_cost = cost;
        p.estimated_rows = rows;
        return p;
    }

    static PhysicalPlan make_filter(Expression predicate, PhysicalPlan input, double cost, size_t rows) {
        PhysicalPlan p;
        p.kind = Kind::Filter;
        p.predicate = std::move(predicate);
        p.input = std::make_unique<PhysicalPlan>(std::move(input));
        p.estimated_cost = cost;
        p.estimated_rows = rows;
        return p;
    }

    static PhysicalPlan make_join(Kind kind, JoinType join_type, Expression condition,
                                   PhysicalPlan left, PhysicalPlan right, double cost, size_t rows) {
        PhysicalPlan p;
        p.kind = kind;
        p.join_type = join_type;
        p.condition = std::move(condition);
        p.left = std::make_unique<PhysicalPlan>(std::move(left));
        p.right = std::make_unique<PhysicalPlan>(std::move(right));
        p.estimated_cost = cost;
        p.estimated_rows = rows;
        return p;
    }

    static PhysicalPlan make_hash_aggregate(std::vector<Expression> group_by,
                                             std::vector<AggregateExpr> aggregates,
                                             PhysicalPlan input, double cost, size_t rows) {
        PhysicalPlan p;
        p.kind = Kind::HashAggregate;
        p.group_by = std::move(group_by);
        p.aggregates = std::move(aggregates);
        p.input = std::make_unique<PhysicalPlan>(std::move(input));
        p.estimated_cost = cost;
        p.estimated_rows = rows;
        return p;
    }

    static PhysicalPlan make_project(std::vector<std::pair<Expression, std::optional<std::string>>> expressions,
                                      PhysicalPlan input, double cost, size_t rows) {
        PhysicalPlan p;
        p.kind = Kind::Project;
        p.expressions = std::move(expressions);
        p.input = std::make_unique<PhysicalPlan>(std::move(input));
        p.estimated_cost = cost;
        p.estimated_rows = rows;
        return p;
    }

    static PhysicalPlan make_sort(std::vector<OrderByItem> order_by, PhysicalPlan input,
                                   double cost, size_t rows) {
        PhysicalPlan p;
        p.kind = Kind::Sort;
        p.order_by = std::move(order_by);
        p.input = std::make_unique<PhysicalPlan>(std::move(input));
        p.estimated_cost = cost;
        p.estimated_rows = rows;
        return p;
    }

    static PhysicalPlan make_limit(size_t count, PhysicalPlan input, double cost, size_t rows) {
        PhysicalPlan p;
        p.kind = Kind::Limit;
        p.count = count;
        p.input = std::make_unique<PhysicalPlan>(std::move(input));
        p.estimated_cost = cost;
        p.estimated_rows = rows;
        return p;
    }
};

} // namespace sql::physical
