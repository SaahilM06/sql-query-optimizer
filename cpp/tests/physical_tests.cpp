// Tests for the physical planner: strategy selection (SeqScan vs IndexScan,
// NestedLoopJoin vs HashJoin) driven by the cost model in
// src/physical/cost.hpp.

#include <algorithm>
#include <cmath>
#include <string>

#include "../src/logical/optimizer.hpp"
#include "../src/logical/planner.hpp"
#include "../src/logical/schema.hpp"
#include "../src/optimizer/cost_model.hpp"
#include "../src/parser/ast.hpp"
#include "../src/parser/lexer.hpp"
#include "../src/parser/parser.hpp"
#include "../src/physical/physical_plan.hpp"
#include "../src/physical/physical_planner.hpp"
#include "../src/statistics/statistics_loader.hpp"
#include "test_framework.hpp"

using namespace sql::parser;
using namespace sql::logical;
using namespace sql::physical;
using namespace sql::statistics;
using sql::optimizer::CostModel;

namespace {

StatisticsCatalog test_stats_catalog() {
    return load_catalog_from_directory(SQL_OPTIMIZER_STATS_DIR);
}

// ── pipeline helper: SQL -> optimized LogicalPlan -> PhysicalPlan ────────────

PhysicalPlan physical_plan_for(const std::string& sql, const Catalog& catalog, const StatisticsCatalog& stats) {
    Lexer lexer(sql);
    std::vector<Token> tokens = lexer.tokenize();
    Parser parser(std::move(tokens));
    Statement stmt = parser.parse();

    LogicalPlanner planner(catalog);
    LogicalPlan logical = planner.plan(std::move(stmt.select));
    LogicalPlan optimized = optimize(std::move(logical), catalog);

    return generate_physical_plan(optimized, catalog, stats);
}

/// True if `node`'s cost is >= every descendant's cost -- cumulative cost
/// should never decrease as you walk up the tree.
bool costs_monotonic(const PhysicalPlan& node) {
    switch (node.kind) {
        case PhysicalPlan::Kind::Filter:
        case PhysicalPlan::Kind::HashAggregate:
        case PhysicalPlan::Kind::Project:
        case PhysicalPlan::Kind::Sort:
        case PhysicalPlan::Kind::Limit:
            return node.estimated_cost.total() >= node.input->estimated_cost.total() && costs_monotonic(*node.input);
        case PhysicalPlan::Kind::NestedLoopJoin:
        case PhysicalPlan::Kind::HashJoin:
            return node.estimated_cost.total() >= node.left->estimated_cost.total() &&
                   node.estimated_cost.total() >= node.right->estimated_cost.total() && costs_monotonic(*node.left) &&
                   costs_monotonic(*node.right);
        case PhysicalPlan::Kind::IndexNestedLoopJoin:
            // Only the outer (left) side's cost is folded in -- the inner
            // side is probed via index instead of scanned, so its own
            // estimated_cost isn't part of this node's cumulative cost.
            return node.estimated_cost.total() >= node.left->estimated_cost.total() && costs_monotonic(*node.left) &&
                   costs_monotonic(*node.right);
        case PhysicalPlan::Kind::SeqScan:
        case PhysicalPlan::Kind::IndexScan:
            return true;
    }
    return false;
}

/// Walk down through single-child wrapper nodes to find the first
/// scan/join/aggregate node.
const PhysicalPlan* skip_wrappers(const PhysicalPlan& node) {
    switch (node.kind) {
        case PhysicalPlan::Kind::Filter:
        case PhysicalPlan::Kind::Project:
        case PhysicalPlan::Kind::Sort:
        case PhysicalPlan::Kind::Limit:
            return skip_wrappers(*node.input);
        default:
            return &node;
    }
}

} // namespace

// ── Scan strategy: SeqScan vs IndexScan ──────────────────────────────────────

// customer_id is indexed on `orders`; an equality predicate on it is
// selective enough (heuristic: ~0.1% of 500,000 rows) that IndexScan beats
// SeqScan + Filter under the cost model.
TEST(equality_on_indexed_column_picks_index_scan) {
    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();
    PhysicalPlan plan = physical_plan_for("SELECT * FROM orders WHERE customer_id = 5", catalog, stats);

    ASSERT_EQ_MSG(plan.kind, PhysicalPlan::Kind::IndexScan, "expected root to be IndexScan");
    ASSERT_EQ(plan.table_name, std::string("orders"));
    ASSERT_EQ(plan.index_column, std::string("customer_id"));
    ASSERT_TRUE_MSG(plan.estimated_rows < 5000, "index scan should estimate far fewer rows than the full table");
}

// `status` has no index in the test catalog, so the planner must fall back
// to SeqScan + Filter -- there is no IndexScan candidate to even consider.
TEST(equality_on_unindexed_column_falls_back_to_seq_scan_filter) {
    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();
    PhysicalPlan plan = physical_plan_for("SELECT * FROM orders WHERE status = 'shipped'", catalog, stats);

    ASSERT_EQ_MSG(plan.kind, PhysicalPlan::Kind::Filter, "expected root to be Filter (no index available)");
    ASSERT_EQ_MSG(plan.input->kind, PhysicalPlan::Kind::SeqScan, "expected Filter's child to be SeqScan");
}

// A non-equality predicate (range comparison) can't use the simple
// equality-lookup IndexScan modeled here, even on an indexed column.
TEST(range_predicate_on_indexed_column_falls_back_to_seq_scan_filter) {
    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();
    PhysicalPlan plan = physical_plan_for("SELECT * FROM orders WHERE customer_id > 5", catalog, stats);

    ASSERT_EQ_MSG(plan.kind, PhysicalPlan::Kind::Filter, "range predicates aren't index-eligible in this model");
    ASSERT_EQ(plan.input->kind, PhysicalPlan::Kind::SeqScan);
}

// ── Join strategy: NestedLoopJoin vs HashJoin ────────────────────────────────

// customers (10,000 rows) equi-joined with orders (500,000 rows): the
// re-scan cost of nested loop join dwarfs hash join's single-pass cost, so
// HashJoin must win.
TEST(large_equi_join_picks_hash_join) {
    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();
    PhysicalPlan plan = physical_plan_for(
        "SELECT c.name, o.total FROM customers c JOIN orders o ON c.id = o.customer_id", catalog, stats);

    const PhysicalPlan* join = skip_wrappers(plan);
    ASSERT_EQ_MSG(join->kind, PhysicalPlan::Kind::HashJoin, "large equi-join should prefer HashJoin");
}

// A non-equi join condition (range comparison) has no HashJoin candidate in
// this model -- NestedLoopJoin is the only option, regardless of cost.
TEST(non_equi_join_only_has_nested_loop_candidate) {
    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();
    PhysicalPlan plan =
        physical_plan_for("SELECT * FROM orders o JOIN products p ON o.total > p.price", catalog, stats);

    const PhysicalPlan* join = skip_wrappers(plan);
    ASSERT_EQ_MSG(join->kind, PhysicalPlan::Kind::NestedLoopJoin, "non-equi join must use NestedLoopJoin");
}

// ── Node-kind mapping for non-strategy operators ─────────────────────────────

TEST(aggregate_maps_to_hash_aggregate) {
    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();
    PhysicalPlan plan = physical_plan_for(
        "SELECT c.name, SUM(o.total) FROM customers c JOIN orders o ON c.id = o.customer_id GROUP BY c.name",
        catalog, stats);

    ASSERT_EQ_MSG(plan.kind, PhysicalPlan::Kind::HashAggregate, "GROUP BY query should produce a HashAggregate root");
}

TEST(sort_and_limit_and_project_present_in_pipeline_query) {
    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();
    PhysicalPlan plan = physical_plan_for(
        "SELECT c.name FROM customers c JOIN orders o ON c.id = o.customer_id "
        "WHERE o.total > 200 ORDER BY c.name LIMIT 10",
        catalog, stats);

    ASSERT_EQ_MSG(plan.kind, PhysicalPlan::Kind::Project, "top node should be Project");
    ASSERT_EQ_MSG(plan.input->kind, PhysicalPlan::Kind::Limit, "Project's child should be Limit");
    ASSERT_EQ_MSG(plan.input->input->kind, PhysicalPlan::Kind::Sort, "Limit's child should be Sort");
    ASSERT_EQ(plan.input->count, static_cast<size_t>(10));
}

// ── Cost sanity ───────────────────────────────────────────────────────────────

TEST(cumulative_cost_is_monotonic_across_the_tree) {
    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();
    PhysicalPlan plan = physical_plan_for(
        "SELECT c.name, o.total, p.name FROM customers c "
        "JOIN orders o ON c.id = o.customer_id "
        "JOIN products p ON o.id = p.id",
        catalog, stats);

    ASSERT_TRUE_MSG(costs_monotonic(plan), "a parent's cumulative cost must never be less than a child's");
    ASSERT_TRUE(plan.estimated_cost.total() > 0.0);
    ASSERT_TRUE(plan.estimated_rows > 0);
}

// CostModel recomputes cost from scratch using the same formulas
// PhysicalPlanner applied incrementally while building the tree -- the two
// should agree, or something has drifted between them.
TEST(cost_model_agrees_with_incremental_planner_cost) {
    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();
    PhysicalPlan plan = physical_plan_for(
        "SELECT c.name, o.total FROM customers c JOIN orders o ON c.id = o.customer_id WHERE o.total > 100", catalog,
        stats);

    CostModel cost_model;
    double recomputed = cost_model.estimate_cost(plan).total();
    double incremental = plan.estimated_cost.total();

    double tolerance = std::max(1.0, incremental * 0.01); // allow tiny float drift
    ASSERT_TRUE_MSG(std::abs(recomputed - incremental) <= tolerance,
                     "CostModel's standalone recomputation should match PhysicalPlanner's incremental cost");
}
