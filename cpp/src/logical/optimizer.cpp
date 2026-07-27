#include "optimizer.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

#include "../parser/ast.hpp"

namespace sql::logical {

using sql::parser::BinaryOperator;
using sql::parser::Literal;

namespace {

// ── Forward declarations (mutual recursion) ──────────────────────────────────

LogicalPlan push_predicates(LogicalPlan plan);
LogicalPlan wrap_filters(LogicalPlan plan, std::vector<Expression> filters);
LogicalPlan push_projections(LogicalPlan plan, const std::unordered_set<std::string>& needed);
LogicalPlan reorder_joins(LogicalPlan plan, const Catalog& catalog);
bool all_inner(const LogicalPlan& plan);
void extract_join_tree(LogicalPlan plan, std::vector<LogicalPlan>& leaves, std::vector<Expression>& conds);
LogicalPlan rebuild_join_tree(std::vector<LogicalPlan> leaves, std::vector<Expression> conds);
LogicalPlan greedy_join_order(std::vector<LogicalPlan> leaves, std::vector<Expression> conds, const Catalog& catalog);
size_t estimate_rows(const LogicalPlan& plan, const Catalog& catalog);
size_t estimate_join_rows(size_t left, size_t right);
std::vector<Expression> split_and(Expression expr);
void visit_tables(const Expression& expr, std::unordered_set<std::string>& out);
std::unordered_set<std::string> referenced_tables(const Expression& expr);
void add_columns_from_expr(const Expression& expr, std::unordered_set<std::string>& out);
std::unordered_set<std::string> columns_in_project_exprs(
    const std::vector<std::pair<Expression, std::optional<std::string>>>& exprs);
void collect_aliases(const LogicalPlan& plan, std::unordered_set<std::string>& out);
std::unordered_set<std::string> scan_aliases(const LogicalPlan& plan);

// ═════════════════════════════════════════════════════════════════════════════
// Rule 1 — Predicate Pushdown
//
// Move Filter nodes as close to their Scan as possible so joins operate on
// the smallest possible intermediate results.
// ═════════════════════════════════════════════════════════════════════════════

LogicalPlan push_predicates(LogicalPlan plan) {
    switch (plan.kind) {
        // The interesting case: a Filter sitting directly above a Join.
        case LogicalPlan::Kind::Filter: {
            Expression predicate = std::move(plan.predicate);
            LogicalPlan input = push_predicates(std::move(*plan.input));

            if (input.kind == LogicalPlan::Kind::Join) {
                JoinType join_type = input.join_type;
                Expression condition = std::move(input.condition);
                LogicalPlan left = std::move(*input.left);
                LogicalPlan right = std::move(*input.right);

                // Split "a AND b AND c" into [a, b, c]
                std::vector<Expression> conjuncts = split_and(std::move(predicate));
                auto left_aliases = scan_aliases(left);
                auto right_aliases = scan_aliases(right);

                std::vector<Expression> left_filters, right_filters, remaining;
                for (auto& conj : conjuncts) {
                    auto tables = referenced_tables(conj);
                    bool on_left = std::any_of(tables.begin(), tables.end(),
                        [&](const std::string& t) { return left_aliases.count(t) > 0; });
                    bool on_right = std::any_of(tables.begin(), tables.end(),
                        [&](const std::string& t) { return right_aliases.count(t) > 0; });

                    if (on_left && !on_right) left_filters.push_back(std::move(conj));
                    else if (!on_left && on_right) right_filters.push_back(std::move(conj));
                    else remaining.push_back(std::move(conj)); // spans both sides -- stays on join
                }

                LogicalPlan new_left = wrap_filters(std::move(left), std::move(left_filters));
                LogicalPlan new_right = wrap_filters(std::move(right), std::move(right_filters));
                LogicalPlan join = LogicalPlan::make_join(join_type, std::move(condition),
                                                           std::move(new_left), std::move(new_right));
                return wrap_filters(std::move(join), std::move(remaining));
            }

            // Filter over anything else -- keep it, recurse already done.
            return LogicalPlan::make_filter(std::move(predicate), std::move(input));
        }

        // Recurse into all other node types.
        case LogicalPlan::Kind::Join: {
            LogicalPlan left = push_predicates(std::move(*plan.left));
            LogicalPlan right = push_predicates(std::move(*plan.right));
            return LogicalPlan::make_join(plan.join_type, std::move(plan.condition), std::move(left), std::move(right));
        }
        case LogicalPlan::Kind::Aggregate: {
            LogicalPlan input = push_predicates(std::move(*plan.input));
            return LogicalPlan::make_aggregate(std::move(plan.group_by), std::move(plan.aggregates), std::move(input));
        }
        case LogicalPlan::Kind::Project: {
            LogicalPlan input = push_predicates(std::move(*plan.input));
            return LogicalPlan::make_project(std::move(plan.expressions), std::move(input));
        }
        case LogicalPlan::Kind::Sort: {
            LogicalPlan input = push_predicates(std::move(*plan.input));
            return LogicalPlan::make_sort(std::move(plan.order_by), std::move(input));
        }
        case LogicalPlan::Kind::Limit: {
            LogicalPlan input = push_predicates(std::move(*plan.input));
            return LogicalPlan::make_limit(plan.count, std::move(input));
        }
        default: // Scan -- nothing to do
            return plan;
    }
}

/// Stack `filters` as Filter nodes on top of `plan` (innermost first).
LogicalPlan wrap_filters(LogicalPlan plan, std::vector<Expression> filters) {
    for (auto& f : filters) {
        plan = LogicalPlan::make_filter(std::move(f), std::move(plan));
    }
    return plan;
}

// ═════════════════════════════════════════════════════════════════════════════
// Rule 2 — Projection Pushdown
//
// Walk the tree top-down carrying the set of column names actually needed.
// At each Scan, record only those columns so the executor skips the rest.
// ═════════════════════════════════════════════════════════════════════════════

LogicalPlan push_projections(LogicalPlan plan, const std::unordered_set<std::string>& needed) {
    switch (plan.kind) {
        // Leaf: record which columns this scan should read.
        case LogicalPlan::Kind::Scan: {
            std::vector<std::string> projected_columns;
            if (!needed.empty()) {
                projected_columns.assign(needed.begin(), needed.end());
            }
            return LogicalPlan::make_scan(std::move(plan.table_name), std::move(plan.alias),
                                           std::move(projected_columns));
        }

        // Project defines its own needed set.
        case LogicalPlan::Kind::Project: {
            auto new_needed = columns_in_project_exprs(plan.expressions);
            new_needed.insert(needed.begin(), needed.end());
            LogicalPlan input = push_projections(std::move(*plan.input), new_needed);
            return LogicalPlan::make_project(std::move(plan.expressions), std::move(input));
        }

        // Filter needs the columns in its predicate.
        case LogicalPlan::Kind::Filter: {
            auto new_needed = needed;
            add_columns_from_expr(plan.predicate, new_needed);
            LogicalPlan input = push_projections(std::move(*plan.input), new_needed);
            return LogicalPlan::make_filter(std::move(plan.predicate), std::move(input));
        }

        // Join needs the columns in its condition on both sides.
        case LogicalPlan::Kind::Join: {
            auto new_needed = needed;
            add_columns_from_expr(plan.condition, new_needed);
            LogicalPlan left = push_projections(std::move(*plan.left), new_needed);
            LogicalPlan right = push_projections(std::move(*plan.right), new_needed);
            return LogicalPlan::make_join(plan.join_type, std::move(plan.condition), std::move(left), std::move(right));
        }

        // Aggregate needs group keys + aggregate arguments.
        case LogicalPlan::Kind::Aggregate: {
            auto new_needed = needed;
            for (const auto& e : plan.group_by) add_columns_from_expr(e, new_needed);
            for (const auto& a : plan.aggregates) add_columns_from_expr(a.arg, new_needed);
            LogicalPlan input = push_projections(std::move(*plan.input), new_needed);
            return LogicalPlan::make_aggregate(std::move(plan.group_by), std::move(plan.aggregates), std::move(input));
        }

        case LogicalPlan::Kind::Sort: {
            auto new_needed = needed;
            for (const auto& item : plan.order_by) add_columns_from_expr(item.expression, new_needed);
            LogicalPlan input = push_projections(std::move(*plan.input), new_needed);
            return LogicalPlan::make_sort(std::move(plan.order_by), std::move(input));
        }

        case LogicalPlan::Kind::Limit: {
            LogicalPlan input = push_projections(std::move(*plan.input), needed);
            return LogicalPlan::make_limit(plan.count, std::move(input));
        }
    }
    throw std::logic_error("unreachable: unknown LogicalPlan::Kind");
}

// ═════════════════════════════════════════════════════════════════════════════
// Rule 3 — Join Reordering
//
// For subtrees made entirely of INNER JOINs, flatten the join tree, then
// greedily rebuild it smallest-output-first using catalog row counts.
// Outer joins (LEFT/RIGHT) preserve their original order.
// ═════════════════════════════════════════════════════════════════════════════

LogicalPlan reorder_joins(LogicalPlan plan, const Catalog& catalog) {
    switch (plan.kind) {
        case LogicalPlan::Kind::Join: {
            // Recurse into children first so inner sub-trees are already optimal.
            LogicalPlan left = reorder_joins(std::move(*plan.left), catalog);
            LogicalPlan right = reorder_joins(std::move(*plan.right), catalog);
            LogicalPlan joined = LogicalPlan::make_join(plan.join_type, std::move(plan.condition),
                                                         std::move(left), std::move(right));

            // Only reorder pure INNER-join subtrees -- outer joins have order semantics.
            if (!all_inner(joined)) {
                return joined;
            }

            std::vector<LogicalPlan> leaves;
            std::vector<Expression> conds;
            extract_join_tree(std::move(joined), leaves, conds);

            if (leaves.size() >= 3) {
                return greedy_join_order(std::move(leaves), std::move(conds), catalog);
            }
            return rebuild_join_tree(std::move(leaves), std::move(conds));
        }

        // Recurse into non-join nodes.
        case LogicalPlan::Kind::Filter: {
            LogicalPlan input = reorder_joins(std::move(*plan.input), catalog);
            return LogicalPlan::make_filter(std::move(plan.predicate), std::move(input));
        }
        case LogicalPlan::Kind::Aggregate: {
            LogicalPlan input = reorder_joins(std::move(*plan.input), catalog);
            return LogicalPlan::make_aggregate(std::move(plan.group_by), std::move(plan.aggregates), std::move(input));
        }
        case LogicalPlan::Kind::Project: {
            LogicalPlan input = reorder_joins(std::move(*plan.input), catalog);
            return LogicalPlan::make_project(std::move(plan.expressions), std::move(input));
        }
        case LogicalPlan::Kind::Sort: {
            LogicalPlan input = reorder_joins(std::move(*plan.input), catalog);
            return LogicalPlan::make_sort(std::move(plan.order_by), std::move(input));
        }
        case LogicalPlan::Kind::Limit: {
            LogicalPlan input = reorder_joins(std::move(*plan.input), catalog);
            return LogicalPlan::make_limit(plan.count, std::move(input));
        }
        default: // Scan
            return plan;
    }
}

/// True when every Join in this subtree is INNER.
bool all_inner(const LogicalPlan& plan) {
    if (plan.kind != LogicalPlan::Kind::Join) return true;
    return plan.join_type == JoinType::Inner && all_inner(*plan.left) && all_inner(*plan.right);
}

/// Flatten a left-deep join tree into (leaf plans, join conditions).
void extract_join_tree(LogicalPlan plan, std::vector<LogicalPlan>& leaves, std::vector<Expression>& conds) {
    if (plan.kind == LogicalPlan::Kind::Join) {
        conds.push_back(std::move(plan.condition));
        LogicalPlan left = std::move(*plan.left);
        LogicalPlan right = std::move(*plan.right);
        extract_join_tree(std::move(left), leaves, conds);
        extract_join_tree(std::move(right), leaves, conds);
    } else {
        leaves.push_back(std::move(plan));
    }
}

/// Rebuild a left-deep join tree from leaves + conditions (used for 2-table case).
LogicalPlan rebuild_join_tree(std::vector<LogicalPlan> leaves, std::vector<Expression> conds) {
    LogicalPlan tree = std::move(leaves.front());
    for (size_t i = 1; i < leaves.size(); ++i) {
        Expression cond;
        if (conds.empty()) {
            cond = Expression::make_literal(Literal::boolean(true));
        } else {
            cond = std::move(conds.front());
            conds.erase(conds.begin());
        }
        tree = LogicalPlan::make_join(JoinType::Inner, std::move(cond), std::move(tree), std::move(leaves[i]));
    }
    return tree;
}

/// Find the pair of nodes connected by a known condition with the smallest join output.
/// Returns (i, j, cond_index) where i < j.
std::tuple<size_t, size_t, std::optional<size_t>> best_pair(
    const std::vector<std::pair<LogicalPlan, size_t>>& nodes, const std::vector<Expression>& conds) {
    size_t best_cost = std::numeric_limits<size_t>::max();
    size_t best_i = 0;
    size_t best_j = 1;
    std::optional<size_t> best_ci;

    for (size_t i = 0; i < nodes.size(); ++i) {
        for (size_t j = i + 1; j < nodes.size(); ++j) {
            auto la = scan_aliases(nodes[i].first);
            auto ra = scan_aliases(nodes[j].first);

            // Look for a condition that links these two relations.
            std::optional<size_t> ci;
            for (size_t k = 0; k < conds.size(); ++k) {
                auto tables = referenced_tables(conds[k]);
                bool on_left = std::any_of(tables.begin(), tables.end(),
                    [&](const std::string& t) { return la.count(t) > 0; });
                bool on_right = std::any_of(tables.begin(), tables.end(),
                    [&](const std::string& t) { return ra.count(t) > 0; });
                if (on_left && on_right) { ci = k; break; }
            }

            // Only consider pairs that share a join condition.
            if (ci.has_value()) {
                size_t cost = estimate_join_rows(nodes[i].second, nodes[j].second);
                if (cost < best_cost) {
                    best_cost = cost;
                    best_i = i;
                    best_j = j;
                    best_ci = ci;
                }
            }
        }
    }

    return {best_i, best_j, best_ci};
}

/// Greedy join ordering: repeatedly pick the pair with the smallest estimated output.
LogicalPlan greedy_join_order(std::vector<LogicalPlan> leaves, std::vector<Expression> conds, const Catalog& catalog) {
    // Attach an estimated row count to each leaf.
    std::vector<std::pair<LogicalPlan, size_t>> nodes;
    nodes.reserve(leaves.size());
    for (auto& n : leaves) {
        size_t r = estimate_rows(n, catalog);
        nodes.emplace_back(std::move(n), r);
    }

    while (nodes.size() > 1) {
        auto [best_i, best_j, cond_idx] = best_pair(nodes, conds);

        Expression cond;
        if (cond_idx.has_value()) {
            cond = std::move(conds[*cond_idx]);
            conds.erase(conds.begin() + static_cast<long>(*cond_idx));
        } else {
            cond = Expression::make_literal(Literal::boolean(true));
        }

        // Remove larger index first so the smaller index stays valid.
        LogicalPlan right_plan = std::move(nodes[best_j].first);
        nodes.erase(nodes.begin() + static_cast<long>(best_j));
        LogicalPlan left_plan = std::move(nodes[best_i].first);
        nodes.erase(nodes.begin() + static_cast<long>(best_i));

        size_t new_rows = estimate_join_rows(estimate_rows(left_plan, catalog), estimate_rows(right_plan, catalog));
        LogicalPlan joined = LogicalPlan::make_join(JoinType::Inner, std::move(cond),
                                                     std::move(left_plan), std::move(right_plan));
        nodes.emplace_back(std::move(joined), new_rows);
    }

    return std::move(nodes.front().first);
}

size_t estimate_rows(const LogicalPlan& plan, const Catalog& catalog) {
    switch (plan.kind) {
        case LogicalPlan::Kind::Scan: {
            const TableSchema* s = catalog.get(plan.table_name);
            return s ? s->stats.row_count : 1000;
        }
        case LogicalPlan::Kind::Filter:
            return std::max<size_t>(estimate_rows(*plan.input, catalog) / 10, 1);
        case LogicalPlan::Kind::Join:
            return estimate_join_rows(estimate_rows(*plan.left, catalog), estimate_rows(*plan.right, catalog));
        default:
            return 1000;
    }
}

/// Equi-join heuristic: 10% of the cartesian product, minimum 1.
size_t estimate_join_rows(size_t left, size_t right) {
    return std::max<size_t>((left * right) / 10, 1);
}

// ═════════════════════════════════════════════════════════════════════════════
// Shared expression helpers
// ═════════════════════════════════════════════════════════════════════════════

/// Flatten `a AND b AND c` -> `[a, b, c]`.
std::vector<Expression> split_and(Expression expr) {
    if (expr.kind == Expression::Kind::BinaryOp && expr.binary_op == BinaryOperator::And) {
        Expression left = std::move(*expr.left);
        Expression right = std::move(*expr.right);
        std::vector<Expression> out = split_and(std::move(left));
        std::vector<Expression> rhs = split_and(std::move(right));
        out.insert(out.end(), std::make_move_iterator(rhs.begin()), std::make_move_iterator(rhs.end()));
        return out;
    }
    std::vector<Expression> out;
    out.push_back(std::move(expr));
    return out;
}

/// Collect every table alias referenced by Column expressions inside `expr`.
void visit_tables(const Expression& expr, std::unordered_set<std::string>& out) {
    switch (expr.kind) {
        case Expression::Kind::Column:
            if (expr.table.has_value()) out.insert(*expr.table);
            break;
        case Expression::Kind::BinaryOp:
            visit_tables(*expr.left, out);
            visit_tables(*expr.right, out);
            break;
        case Expression::Kind::UnaryOp:
            visit_tables(*expr.operand, out);
            break;
        case Expression::Kind::Function:
            for (const auto& a : expr.args) visit_tables(a, out);
            break;
        case Expression::Kind::Literal:
        case Expression::Kind::Wildcard:
            break;
    }
}

std::unordered_set<std::string> referenced_tables(const Expression& expr) {
    std::unordered_set<std::string> out;
    visit_tables(expr, out);
    return out;
}

/// Add every column name (without table qualifier) from `expr` into `out`.
void add_columns_from_expr(const Expression& expr, std::unordered_set<std::string>& out) {
    switch (expr.kind) {
        case Expression::Kind::Column:
            out.insert(expr.column);
            break;
        case Expression::Kind::BinaryOp:
            add_columns_from_expr(*expr.left, out);
            add_columns_from_expr(*expr.right, out);
            break;
        case Expression::Kind::UnaryOp:
            add_columns_from_expr(*expr.operand, out);
            break;
        case Expression::Kind::Function:
            for (const auto& a : expr.args) add_columns_from_expr(a, out);
            break;
        case Expression::Kind::Literal:
        case Expression::Kind::Wildcard:
            break;
    }
}

std::unordered_set<std::string> columns_in_project_exprs(
    const std::vector<std::pair<Expression, std::optional<std::string>>>& exprs) {
    std::unordered_set<std::string> out;
    for (const auto& pr : exprs) add_columns_from_expr(pr.first, out);
    return out;
}

/// Collect the alias (or bare table name) from every Scan reachable under `plan`.
void collect_aliases(const LogicalPlan& plan, std::unordered_set<std::string>& out) {
    switch (plan.kind) {
        case LogicalPlan::Kind::Scan:
            out.insert(plan.alias.value_or(plan.table_name));
            break;
        case LogicalPlan::Kind::Join:
            collect_aliases(*plan.left, out);
            collect_aliases(*plan.right, out);
            break;
        case LogicalPlan::Kind::Filter:
        case LogicalPlan::Kind::Aggregate:
        case LogicalPlan::Kind::Project:
        case LogicalPlan::Kind::Sort:
        case LogicalPlan::Kind::Limit:
            collect_aliases(*plan.input, out);
            break;
    }
}

std::unordered_set<std::string> scan_aliases(const LogicalPlan& plan) {
    std::unordered_set<std::string> out;
    collect_aliases(plan, out);
    return out;
}

} // namespace

// ── Public entry point ────────────────────────────────────────────────────────

LogicalPlan optimize(LogicalPlan plan, const Catalog& catalog) {
    plan = push_predicates(std::move(plan));
    plan = push_projections(std::move(plan), {});
    return reorder_joins(std::move(plan), catalog);
}

} // namespace sql::logical
