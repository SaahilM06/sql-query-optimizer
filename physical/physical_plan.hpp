#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../logical/logical_plan.hpp"
#include "../parser/ast.hpp"
#include "cost.hpp"

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
// `cardinality_reasoning`/`cardinality_confidence` carry the
// CardinalityEstimator's explanation for `estimated_rows` (see
// optimizer::Estimate), so an annotated plan can be printed showing not
// just a row count but where it came from.
//
// Deep-copyable (like Expression), not move-only like LogicalPlan: the
// join-order DP search (see optimizer::JoinEnumerator) memoizes the best
// plan for each relation subset and reuses it as a building block for many
// candidate parent plans, only one of which ultimately wins -- that
// requires copying a subplan without disturbing the memoized original.
class PhysicalPlan {
public:
    enum class Kind {
        SeqScan,
        IndexScan,
        Filter,
        NestedLoopJoin,
        HashJoin,
        IndexNestedLoopJoin,
        HashAggregate,
        Project,
        Sort,
        Limit,
        // A leaf that never comes out of normal planning (join_enumerator/
        // PhysicalPlanner never produce it) -- distributed::Coordinator
        // synthesizes it after the fact, replacing a subtree it has already
        // computed elsewhere (broadcast or shuffle), to ship a worker a
        // plan that reads that data from an injected row set instead of
        // scanning a table. Reuses table_name (informational, e.g.
        // "broadcast:products") and count (the slot id build_executor's
        // external_rows map is keyed by). projected_columns holds each
        // column's identity as "table.column" or a bare "column" (see
        // RowSchema::qualified_name) rather than one uniform alias, since a
        // HashAggregate's output can mix qualified GROUP BY columns with
        // unqualified aggregate result columns.
        ExternalRows,
    };

    Kind kind = Kind::SeqScan;
    cost::Cost estimated_cost;   // cumulative cost of this subtree
    size_t estimated_rows = 0;   // cumulative output row estimate
    std::string cardinality_reasoning;
    double cardinality_confidence = 1.0;

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
    PhysicalPlan(PhysicalPlan&&) noexcept = default;
    PhysicalPlan& operator=(PhysicalPlan&&) noexcept = default;

    PhysicalPlan(const PhysicalPlan& other) { *this = other; }

    PhysicalPlan& operator=(const PhysicalPlan& other) {
        if (this == &other) return *this;
        kind = other.kind;
        estimated_cost = other.estimated_cost;
        estimated_rows = other.estimated_rows;
        cardinality_reasoning = other.cardinality_reasoning;
        cardinality_confidence = other.cardinality_confidence;

        table_name = other.table_name;
        alias = other.alias;
        projected_columns = other.projected_columns;
        index_column = other.index_column;
        index_probe_value = other.index_probe_value;

        predicate = other.predicate;
        input = other.input ? std::make_unique<PhysicalPlan>(*other.input) : nullptr;

        join_type = other.join_type;
        condition = other.condition;
        left = other.left ? std::make_unique<PhysicalPlan>(*other.left) : nullptr;
        right = other.right ? std::make_unique<PhysicalPlan>(*other.right) : nullptr;

        group_by = other.group_by;
        aggregates = other.aggregates;
        expressions = other.expressions;
        order_by = other.order_by;
        count = other.count;
        return *this;
    }

    static PhysicalPlan make_seq_scan(std::string table_name, std::optional<std::string> alias,
                                       std::vector<std::string> projected_columns, cost::Cost cost, size_t rows,
                                       std::string reasoning = "", double confidence = 1.0) {
        PhysicalPlan p;
        p.kind = Kind::SeqScan;
        p.table_name = std::move(table_name);
        p.alias = std::move(alias);
        p.projected_columns = std::move(projected_columns);
        p.estimated_cost = cost;
        p.estimated_rows = rows;
        p.cardinality_reasoning = std::move(reasoning);
        p.cardinality_confidence = confidence;
        return p;
    }

    static PhysicalPlan make_index_scan(std::string table_name, std::optional<std::string> alias,
                                         std::vector<std::string> projected_columns, std::string index_column,
                                         Expression probe_value, cost::Cost cost, size_t rows,
                                         std::string reasoning = "", double confidence = 1.0) {
        PhysicalPlan p;
        p.kind = Kind::IndexScan;
        p.table_name = std::move(table_name);
        p.alias = std::move(alias);
        p.projected_columns = std::move(projected_columns);
        p.index_column = std::move(index_column);
        p.index_probe_value = std::move(probe_value);
        p.estimated_cost = cost;
        p.estimated_rows = rows;
        p.cardinality_reasoning = std::move(reasoning);
        p.cardinality_confidence = confidence;
        return p;
    }

    static PhysicalPlan make_filter(Expression predicate, PhysicalPlan input, cost::Cost cost, size_t rows,
                                     std::string reasoning = "", double confidence = 1.0) {
        PhysicalPlan p;
        p.kind = Kind::Filter;
        p.predicate = std::move(predicate);
        p.input = std::make_unique<PhysicalPlan>(std::move(input));
        p.estimated_cost = cost;
        p.estimated_rows = rows;
        p.cardinality_reasoning = std::move(reasoning);
        p.cardinality_confidence = confidence;
        return p;
    }

    static PhysicalPlan make_join(Kind kind, JoinType join_type, Expression condition, PhysicalPlan left,
                                   PhysicalPlan right, cost::Cost cost, size_t rows, std::string reasoning = "",
                                   double confidence = 1.0) {
        PhysicalPlan p;
        p.kind = kind;
        p.join_type = join_type;
        p.condition = std::move(condition);
        p.left = std::make_unique<PhysicalPlan>(std::move(left));
        p.right = std::make_unique<PhysicalPlan>(std::move(right));
        p.estimated_cost = cost;
        p.estimated_rows = rows;
        p.cardinality_reasoning = std::move(reasoning);
        p.cardinality_confidence = confidence;
        return p;
    }

    static PhysicalPlan make_hash_aggregate(std::vector<Expression> group_by, std::vector<AggregateExpr> aggregates,
                                             PhysicalPlan input, cost::Cost cost, size_t rows,
                                             std::string reasoning = "", double confidence = 1.0) {
        PhysicalPlan p;
        p.kind = Kind::HashAggregate;
        p.group_by = std::move(group_by);
        p.aggregates = std::move(aggregates);
        p.input = std::make_unique<PhysicalPlan>(std::move(input));
        p.estimated_cost = cost;
        p.estimated_rows = rows;
        p.cardinality_reasoning = std::move(reasoning);
        p.cardinality_confidence = confidence;
        return p;
    }

    static PhysicalPlan make_project(std::vector<std::pair<Expression, std::optional<std::string>>> expressions,
                                      PhysicalPlan input, cost::Cost cost, size_t rows, std::string reasoning = "",
                                      double confidence = 1.0) {
        PhysicalPlan p;
        p.kind = Kind::Project;
        p.expressions = std::move(expressions);
        p.input = std::make_unique<PhysicalPlan>(std::move(input));
        p.estimated_cost = cost;
        p.estimated_rows = rows;
        p.cardinality_reasoning = std::move(reasoning);
        p.cardinality_confidence = confidence;
        return p;
    }

    static PhysicalPlan make_sort(std::vector<OrderByItem> order_by, PhysicalPlan input, cost::Cost cost, size_t rows,
                                   std::string reasoning = "", double confidence = 1.0) {
        PhysicalPlan p;
        p.kind = Kind::Sort;
        p.order_by = std::move(order_by);
        p.input = std::make_unique<PhysicalPlan>(std::move(input));
        p.estimated_cost = cost;
        p.estimated_rows = rows;
        p.cardinality_reasoning = std::move(reasoning);
        p.cardinality_confidence = confidence;
        return p;
    }

    static PhysicalPlan make_limit(size_t count, PhysicalPlan input, cost::Cost cost, size_t rows,
                                    std::string reasoning = "", double confidence = 1.0) {
        PhysicalPlan p;
        p.kind = Kind::Limit;
        p.count = count;
        p.input = std::make_unique<PhysicalPlan>(std::move(input));
        p.estimated_cost = cost;
        p.estimated_rows = rows;
        p.cardinality_reasoning = std::move(reasoning);
        p.cardinality_confidence = confidence;
        return p;
    }

    // `projected_columns` entries are each "table.column" or a bare
    // "column" -- see the ExternalRows Kind comment above.
    static PhysicalPlan make_external_rows(std::string table_name, std::vector<std::string> projected_columns,
                                            size_t slot_id, size_t rows) {
        PhysicalPlan p;
        p.kind = Kind::ExternalRows;
        p.table_name = std::move(table_name);
        p.projected_columns = std::move(projected_columns);
        p.count = slot_id;
        p.estimated_rows = rows;
        return p;
    }
};

} // namespace sql::physical
