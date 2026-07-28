// Tests for the logical optimizer. Ported 1:1 from the original Rust
// `tests/optimizer_tests.rs`.
//
// Each test:
//   1. Parses a SQL string into an AST
//   2. Runs the logical planner to get a raw LogicalPlan
//   3. Runs optimize() to get the rewritten plan
//   4. Asserts the tree has the expected shape

#include <iostream>
#include <string>

#include "../logical/logical_plan.hpp"
#include "../logical/optimizer.hpp"
#include "../logical/planner.hpp"
#include "../logical/schema.hpp"
#include "../parser/ast.hpp"
#include "../parser/lexer.hpp"
#include "../parser/parser.hpp"
#include "test_framework.hpp"

using namespace sql::parser;
using namespace sql::logical;

namespace {

// ── pipeline helpers ─────────────────────────────────────────────────────────

LogicalPlan plan(const std::string& sql) {
    Lexer lexer(sql);
    std::vector<Token> tokens = lexer.tokenize();
    Parser parser(std::move(tokens));
    Statement stmt = parser.parse();
    Catalog catalog = Catalog::with_test_tables();
    LogicalPlanner planner(catalog);
    return planner.plan(std::move(stmt.select));
}

LogicalPlan optimized(const std::string& sql) {
    LogicalPlan raw = plan(sql);
    Catalog catalog = Catalog::with_test_tables();
    return optimize(std::move(raw), catalog);
}

// ── pretty-printer ────────────────────────────────────────────────────────────

const char* kind_name(LogicalPlan::Kind k) {
    switch (k) {
        case LogicalPlan::Kind::Scan: return "Scan";
        case LogicalPlan::Kind::Filter: return "Filter";
        case LogicalPlan::Kind::Join: return "Join";
        case LogicalPlan::Kind::Aggregate: return "Aggregate";
        case LogicalPlan::Kind::Project: return "Project";
        case LogicalPlan::Kind::Sort: return "Sort";
        case LogicalPlan::Kind::Limit: return "Limit";
    }
    return "?";
}

void print_plan(const LogicalPlan& node, int depth) {
    std::string pad(static_cast<size_t>(depth) * 2, ' ');
    switch (node.kind) {
        case LogicalPlan::Kind::Scan: {
            std::string alias_str = node.alias.value_or("-");
            std::string cols = node.projected_columns.empty() ? "*" : "";
            for (size_t i = 0; i < node.projected_columns.size(); ++i) {
                if (i > 0) cols += ", ";
                cols += node.projected_columns[i];
            }
            std::cout << pad << "Scan(" << node.table_name << " AS " << alias_str << ") [cols: " << cols << "]\n";
            break;
        }
        case LogicalPlan::Kind::Filter:
            std::cout << pad << "Filter(...)\n";
            print_plan(*node.input, depth + 1);
            break;
        case LogicalPlan::Kind::Join:
            std::cout << pad << "Join(...)\n";
            print_plan(*node.left, depth + 1);
            print_plan(*node.right, depth + 1);
            break;
        case LogicalPlan::Kind::Aggregate:
            std::cout << pad << "Aggregate(...)\n";
            print_plan(*node.input, depth + 1);
            break;
        case LogicalPlan::Kind::Project:
            std::cout << pad << "Project(...)\n";
            print_plan(*node.input, depth + 1);
            break;
        case LogicalPlan::Kind::Sort:
            std::cout << pad << "Sort(...)\n";
            print_plan(*node.input, depth + 1);
            break;
        case LogicalPlan::Kind::Limit:
            std::cout << pad << "Limit(" << node.count << ")\n";
            print_plan(*node.input, depth + 1);
            break;
    }
}

// ── tree-inspection helpers ───────────────────────────────────────────────────

/// Count Filter nodes encountered while walking *down toward* the first Join.
/// Recurses through Project / Sort / Limit / Aggregate wrappers transparently.
size_t filters_above_join(const LogicalPlan& node) {
    switch (node.kind) {
        case LogicalPlan::Kind::Filter:
            return 1 + filters_above_join(*node.input);
        case LogicalPlan::Kind::Join:
            return 0;
        case LogicalPlan::Kind::Project:
        case LogicalPlan::Kind::Aggregate:
        case LogicalPlan::Kind::Sort:
        case LogicalPlan::Kind::Limit:
            return filters_above_join(*node.input);
        case LogicalPlan::Kind::Scan:
            return 0;
    }
    return 0;
}

/// Count ALL Filter nodes anywhere in a subtree.
size_t count_filters(const LogicalPlan& node) {
    switch (node.kind) {
        case LogicalPlan::Kind::Filter:
            return 1 + count_filters(*node.input);
        case LogicalPlan::Kind::Join:
            return count_filters(*node.left) + count_filters(*node.right);
        case LogicalPlan::Kind::Project:
        case LogicalPlan::Kind::Aggregate:
        case LogicalPlan::Kind::Sort:
        case LogicalPlan::Kind::Limit:
            return count_filters(*node.input);
        case LogicalPlan::Kind::Scan:
            return 0;
    }
    return 0;
}

/// Count Filter nodes that live *inside* a Join's children (strictly below it).
/// First navigates down to the Join, then counts inside its left/right subtrees.
size_t filters_below_join(const LogicalPlan& node) {
    switch (node.kind) {
        case LogicalPlan::Kind::Join:
            return count_filters(*node.left) + count_filters(*node.right);
        case LogicalPlan::Kind::Filter:
        case LogicalPlan::Kind::Project:
        case LogicalPlan::Kind::Aggregate:
        case LogicalPlan::Kind::Sort:
        case LogicalPlan::Kind::Limit:
            return filters_below_join(*node.input);
        case LogicalPlan::Kind::Scan:
            return 0;
    }
    return 0;
}

/// True if `kind` appears anywhere in the plan tree.
bool contains_node(const LogicalPlan& node, LogicalPlan::Kind kind) {
    if (node.kind == kind) return true;
    switch (node.kind) {
        case LogicalPlan::Kind::Join:
            return contains_node(*node.left, kind) || contains_node(*node.right, kind);
        case LogicalPlan::Kind::Filter:
        case LogicalPlan::Kind::Project:
        case LogicalPlan::Kind::Aggregate:
        case LogicalPlan::Kind::Sort:
        case LogicalPlan::Kind::Limit:
            return contains_node(*node.input, kind);
        case LogicalPlan::Kind::Scan:
            return false;
    }
    return false;
}

/// Return the (left, right) children of the first Join found by walking
/// down through Project / Sort / Limit wrappers from the root.
const LogicalPlan* top_join_left(const LogicalPlan& node) {
    switch (node.kind) {
        case LogicalPlan::Kind::Join:
            return node.left.get();
        case LogicalPlan::Kind::Project:
        case LogicalPlan::Kind::Sort:
        case LogicalPlan::Kind::Limit:
            return top_join_left(*node.input);
        default:
            return nullptr;
    }
}

const LogicalPlan* top_join_right(const LogicalPlan& node) {
    switch (node.kind) {
        case LogicalPlan::Kind::Join:
            return node.right.get();
        case LogicalPlan::Kind::Project:
        case LogicalPlan::Kind::Sort:
        case LogicalPlan::Kind::Limit:
            return top_join_right(*node.input);
        default:
            return nullptr;
    }
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Rule 1 — Predicate Pushdown
// ═════════════════════════════════════════════════════════════════════════════

// Before: Filter(o.total > 100) -> Join -> [Scan(c), Scan(o)]
// After:  Join -> [Scan(c), Filter(o.total > 100) -> Scan(o)]
//
// The filter only references 'o' (orders) so it should be pushed to the
// right side of the join -- no filters should remain above the join.
TEST(predicate_pushed_below_join) {
    std::string sql =
        "SELECT c.name "
        "FROM customers c "
        "JOIN orders o ON c.id = o.customer_id "
        "WHERE o.total > 100";

    LogicalPlan before = plan(sql);
    LogicalPlan after = optimized(sql);

    std::cout << "\n--- BEFORE ---\n";
    print_plan(before, 0);
    std::cout << "\n--- AFTER ---\n";
    print_plan(after, 0);

    ASSERT_EQ_MSG(filters_above_join(after), static_cast<size_t>(0),
                  "predicate should have been pushed below the join -- none should remain above it");
    ASSERT_EQ_MSG(filters_below_join(after), static_cast<size_t>(1),
                  "expected exactly one filter below the join (on the orders side)");
}

// WHERE has two conjuncts -- one per table.
// Both should be pushed to their respective sides.
TEST(predicate_both_sides_pushed) {
    std::string sql =
        "SELECT c.name "
        "FROM customers c "
        "JOIN orders o ON c.id = o.customer_id "
        "WHERE c.country = 'US' AND o.total > 500";

    LogicalPlan before = plan(sql);
    LogicalPlan after = optimized(sql);

    std::cout << "\n--- BEFORE ---\n";
    print_plan(before, 0);
    std::cout << "\n--- AFTER ---\n";
    print_plan(after, 0);

    ASSERT_EQ_MSG(filters_above_join(after), static_cast<size_t>(0), "both predicates should be pushed below the join");
    ASSERT_EQ_MSG(filters_below_join(after), static_cast<size_t>(2), "expected one filter on each side of the join");
}

// A predicate that references columns from BOTH tables cannot be pushed
// to either side -- it must stay at or above the join.
TEST(cross_table_predicate_stays_on_join) {
    std::string sql =
        "SELECT c.name "
        "FROM customers c "
        "JOIN orders o ON c.id = o.customer_id "
        "WHERE c.id = o.customer_id";

    LogicalPlan after = optimized(sql);

    std::cout << "\n--- AFTER ---\n";
    print_plan(after, 0);

    ASSERT_EQ_MSG(filters_below_join(after), static_cast<size_t>(0),
                  "cross-table predicate cannot be pushed to either side");
}

// ═════════════════════════════════════════════════════════════════════════════
// Rule 2 — Projection Pushdown
// ═════════════════════════════════════════════════════════════════════════════

// Only 'total' is needed. The Scan should record only that column,
// not all four columns in the orders table.
TEST(projection_pushdown_trims_scan_columns) {
    std::string sql =
        "SELECT o.total "
        "FROM orders o "
        "WHERE o.total > 100";

    LogicalPlan after = optimized(sql);

    std::cout << "\n--- AFTER ---\n";
    print_plan(after, 0);

    const LogicalPlan* node = &after;
    while (node->kind != LogicalPlan::Kind::Scan) {
        ASSERT_TRUE_MSG(node->input != nullptr, "expected a Scan node");
        node = node->input.get();
    }

    ASSERT_TRUE_MSG(!node->projected_columns.empty(),
                     "projection pushdown should restrict the scan to needed columns");
    bool has_total = false;
    for (const auto& c : node->projected_columns) {
        if (c == "total") has_total = true;
    }
    ASSERT_TRUE_MSG(has_total, "scan must include 'total'");
}

// SELECT * should leave projected_columns empty (= all columns).
TEST(projection_pushdown_wildcard_keeps_all_columns) {
    std::string sql = "SELECT * FROM orders";

    LogicalPlan after = optimized(sql);

    std::cout << "\n--- AFTER ---\n";
    print_plan(after, 0);

    ASSERT_EQ_MSG(after.kind, LogicalPlan::Kind::Scan, "expected Scan at root");
    ASSERT_TRUE_MSG(after.projected_columns.empty(), "SELECT * should leave projected_columns empty (read all columns)");
}

// ═════════════════════════════════════════════════════════════════════════════
// Rule 3 — Join Reordering
// ═════════════════════════════════════════════════════════════════════════════

// Catalog row counts:
//   customers  ->  10,000
//   orders     -> 500,000
//   products   ->   2,000
//
// Naive order (from SQL): customers -> orders -> products
// Optimal order:          products (2k) x customers (10k) first, then x orders
//
// After reordering, the DFS scan order should differ from the original.
TEST(join_reorder_three_tables) {
    std::string sql =
        "SELECT c.name, o.total, p.name "
        "FROM customers c "
        "JOIN orders   o ON c.id  = o.customer_id "
        "JOIN products p ON o.id  = p.id";

    LogicalPlan before = plan(sql);
    LogicalPlan after = optimized(sql);

    std::cout << "\n--- BEFORE ---\n";
    print_plan(before, 0);
    std::cout << "\n--- AFTER ---\n";
    print_plan(after, 0);

    // The plan must still contain a Join after reordering.
    ASSERT_TRUE_MSG(contains_node(after, LogicalPlan::Kind::Join), "optimized plan should still contain a Join node");

    // The optimizer changed:
    //   BEFORE: (customers x orders) x products   -- left child of top join is a Join
    //   AFTER:  customers x (orders x products)   -- right child of top join is a Join
    const LogicalPlan* before_l = top_join_left(before);
    const LogicalPlan* before_r = top_join_right(before);
    ASSERT_TRUE_MSG(before_l != nullptr && before_r != nullptr, "no Join in before plan");

    const LogicalPlan* after_l = top_join_left(after);
    const LogicalPlan* after_r = top_join_right(after);
    ASSERT_TRUE_MSG(after_l != nullptr && after_r != nullptr, "no Join in after plan");

    bool before_nested_left = before_l->kind == LogicalPlan::Kind::Join;
    bool after_nested_right = after_r->kind == LogicalPlan::Kind::Join;

    std::cout << "BEFORE: left-child is Join = " << before_nested_left << "\n";
    std::cout << "AFTER:  right-child is Join = " << after_nested_right << "\n";

    // Before reordering: left-deep tree (left child is the inner join).
    ASSERT_TRUE_MSG(before_nested_left, "original plan should be left-deep: left child of top join is a Join");

    // After reordering: right child should be the inner join (orders x products first).
    ASSERT_TRUE_MSG(after_nested_right, "after reordering, orders x products should be the right (inner) join");
}

// Two-table join: nothing to reorder, but the plan must remain valid.
TEST(join_reorder_two_tables_unchanged) {
    std::string sql =
        "SELECT c.name, o.total "
        "FROM customers c "
        "JOIN orders o ON c.id = o.customer_id";

    LogicalPlan after = optimized(sql);

    std::cout << "\n--- AFTER ---\n";
    print_plan(after, 0);

    ASSERT_TRUE_MSG(contains_node(after, LogicalPlan::Kind::Join),
                     "two-table join must still contain a Join node after optimization");
}

// ═════════════════════════════════════════════════════════════════════════════
// End-to-end: all three passes together
// ═════════════════════════════════════════════════════════════════════════════

// Aggregation + join + WHERE + GROUP BY.
// The WHERE filter should be pushed below the join.
// The top-level node should be Aggregate (planner doesn't add Project when
// aggregates are present).
TEST(full_pipeline_aggregate_query) {
    std::string sql =
        "SELECT c.name, SUM(o.total) "
        "FROM customers c "
        "JOIN orders o ON c.id = o.customer_id "
        "WHERE o.total > 100 "
        "GROUP BY c.name";

    LogicalPlan before = plan(sql);
    LogicalPlan after = optimized(sql);

    std::cout << "\n=== BEFORE ===\n";
    print_plan(before, 0);
    std::cout << "\n=== AFTER ===\n";
    print_plan(after, 0);

    // Planner skips Project when there are aggregates -- top node is Aggregate.
    ASSERT_EQ_MSG(after.kind, LogicalPlan::Kind::Aggregate, "top node should be Aggregate for a GROUP BY query");

    // WHERE filter on o.total should have been pushed below the join.
    ASSERT_EQ_MSG(filters_above_join(after), static_cast<size_t>(0), "WHERE predicate should be pushed below the join");
    ASSERT_EQ_MSG(filters_below_join(after), static_cast<size_t>(1),
                  "expected one filter below the join (on the orders side)");
}

// ORDER BY + LIMIT + WHERE -- correct wrapper order and filter pushdown.
TEST(full_pipeline_limit_orderby) {
    std::string sql =
        "SELECT c.name "
        "FROM customers c "
        "JOIN orders o ON c.id = o.customer_id "
        "WHERE o.total > 200 "
        "ORDER BY c.name "
        "LIMIT 10";

    LogicalPlan before = plan(sql);
    LogicalPlan after = optimized(sql);

    std::cout << "\n=== BEFORE ===\n";
    print_plan(before, 0);
    std::cout << "\n=== AFTER ===\n";
    print_plan(after, 0);

    // Plan structure: Project -> Limit -> Sort -> Join -> ...
    // The top-level node is Project (wraps the column list).
    ASSERT_EQ_MSG(after.kind, LogicalPlan::Kind::Project, "top node should be Project");

    // Limit and Sort must both be present somewhere in the tree.
    ASSERT_TRUE_MSG(contains_node(after, LogicalPlan::Kind::Limit), "plan must contain a Limit node");
    ASSERT_TRUE_MSG(contains_node(after, LogicalPlan::Kind::Sort), "plan must contain a Sort node");

    // WHERE predicate should be pushed below the join.
    ASSERT_EQ_MSG(filters_above_join(after), static_cast<size_t>(0), "WHERE predicate should be pushed below the join");
    ASSERT_EQ_MSG(filters_below_join(after), static_cast<size_t>(1),
                  "expected one filter below the join (on the orders side)");
}
