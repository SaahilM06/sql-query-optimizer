// Tests for Part 3: join graph extraction, access path generation, and
// the Selinger-style DP join-order search (join order + physical algorithm
// chosen together).

#include <algorithm>
#include <set>
#include <string>

#include "../logical/logical_plan.hpp"
#include "../logical/optimizer.hpp"
#include "../logical/planner.hpp"
#include "../logical/schema.hpp"
#include "../optimizer/access_path_generator.hpp"
#include "../optimizer/cardinality_estimator.hpp"
#include "../optimizer/join_enumerator.hpp"
#include "../optimizer/join_graph.hpp"
#include "../parser/ast.hpp"
#include "../parser/lexer.hpp"
#include "../parser/parser.hpp"
#include "../physical/physical_plan.hpp"
#include "../physical/physical_planner.hpp"
#include "../statistics/statistics_loader.hpp"
#include "test_framework.hpp"

using namespace sql::parser;
using namespace sql::logical;
using namespace sql::physical;
using namespace sql::statistics;
using namespace sql::optimizer;

namespace {

StatisticsCatalog test_stats_catalog() { return load_catalog_from_directory(SQL_OPTIMIZER_STATS_DIR); }

Expression col(const std::string& table, const std::string& name) { return Expression::make_column(table, name); }
Expression int_lit(int64_t v) { return Expression::make_literal(Literal::integer(v)); }
Expression str_lit(const std::string& v) { return Expression::make_literal(Literal::str(v)); }
Expression cmp(Expression l, BinaryOperator op, Expression r) { return Expression::make_binary_op(std::move(l), op, std::move(r)); }

LogicalPlan logical_plan_for(const std::string& sql, const Catalog& catalog) {
    Lexer lexer(sql);
    Parser parser(lexer.tokenize());
    Statement stmt = parser.parse();
    LogicalPlanner planner(catalog);
    LogicalPlan plan = planner.plan(std::move(stmt.select));
    return optimize(std::move(plan), catalog);
}

PhysicalPlan physical_plan_for(const std::string& sql, const Catalog& catalog, const StatisticsCatalog& stats) {
    LogicalPlan optimized = logical_plan_for(sql, catalog);
    return generate_physical_plan(optimized, catalog, stats);
}

// Unwraps Project/Sort/Limit/Aggregate/Filter down to the core join (or
// scan) node -- mirroring what PhysicalPlanner::plan_node actually does
// (recurse through those wrappers, re-checking is_join_search_candidate at
// each level) so these isolated graph-extraction tests see the same
// subtree the real planner would hand to build_join_graph, rather than the
// whole statement's plan (which LogicalPlanner almost always wraps in a
// Project).
const LogicalPlan& find_join_search_root(const LogicalPlan& node) {
    if (is_join_search_candidate(node)) return node;
    switch (node.kind) {
        case LogicalPlan::Kind::Project:
        case LogicalPlan::Kind::Sort:
        case LogicalPlan::Kind::Limit:
        case LogicalPlan::Kind::Aggregate:
        case LogicalPlan::Kind::Filter:
            return find_join_search_root(*node.input);
        default:
            return node;
    }
}

// Collects every base-table name at the leaves of a physical plan, in
// left-to-right (DFS) order -- this is the plan's actual scan order,
// independent of which physical join algorithms were chosen.
void collect_leaf_tables(const PhysicalPlan& node, std::vector<std::string>& out) {
    switch (node.kind) {
        case PhysicalPlan::Kind::SeqScan:
        case PhysicalPlan::Kind::IndexScan:
            out.push_back(node.table_name);
            break;
        case PhysicalPlan::Kind::Filter:
        case PhysicalPlan::Kind::HashAggregate:
        case PhysicalPlan::Kind::Project:
        case PhysicalPlan::Kind::Sort:
        case PhysicalPlan::Kind::Limit:
            collect_leaf_tables(*node.input, out);
            break;
        case PhysicalPlan::Kind::NestedLoopJoin:
        case PhysicalPlan::Kind::HashJoin:
        case PhysicalPlan::Kind::IndexNestedLoopJoin:
            collect_leaf_tables(*node.left, out);
            collect_leaf_tables(*node.right, out);
            break;
    }
}

// True if some join node's two children are together exactly {a, b} --
// i.e. a and b were joined directly to each other at some point in the
// tree, regardless of where that join sits overall.
bool joined_directly(const PhysicalPlan& node, const std::string& a, const std::string& b) {
    bool is_join = node.kind == PhysicalPlan::Kind::NestedLoopJoin || node.kind == PhysicalPlan::Kind::HashJoin ||
                   node.kind == PhysicalPlan::Kind::IndexNestedLoopJoin;
    if (is_join) {
        std::vector<std::string> left_tables, right_tables;
        collect_leaf_tables(*node.left, left_tables);
        collect_leaf_tables(*node.right, right_tables);
        std::set<std::string> combined(left_tables.begin(), left_tables.end());
        combined.insert(right_tables.begin(), right_tables.end());
        if (left_tables.size() == 1 && right_tables.size() == 1 && combined.count(a) && combined.count(b)) {
            return true;
        }
    }

    switch (node.kind) {
        case PhysicalPlan::Kind::Filter:
        case PhysicalPlan::Kind::HashAggregate:
        case PhysicalPlan::Kind::Project:
        case PhysicalPlan::Kind::Sort:
        case PhysicalPlan::Kind::Limit:
            return joined_directly(*node.input, a, b);
        case PhysicalPlan::Kind::NestedLoopJoin:
        case PhysicalPlan::Kind::HashJoin:
        case PhysicalPlan::Kind::IndexNestedLoopJoin:
            return joined_directly(*node.left, a, b) || joined_directly(*node.right, a, b);
        default:
            return false;
    }
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Milestone 3.1 — Join graph extraction
// ═════════════════════════════════════════════════════════════════════════════

TEST(join_graph_extracts_relations_and_edges) {
    Catalog catalog = Catalog::with_test_tables();
    LogicalPlan plan = logical_plan_for(
        "SELECT c.name FROM customers c "
        "JOIN orders o ON c.id = o.customer_id "
        "JOIN products p ON o.product_id = p.id",
        catalog);

    const LogicalPlan& root = find_join_search_root(plan);
    ASSERT_TRUE(is_join_search_candidate(root));
    JoinGraphExtraction extraction = build_join_graph(root);

    ASSERT_EQ(extraction.graph.relations.size(), static_cast<size_t>(3));
    ASSERT_TRUE(extraction.graph.find_relation("c").has_value());
    ASSERT_TRUE(extraction.graph.find_relation("o").has_value());
    ASSERT_TRUE(extraction.graph.find_relation("p").has_value());
    ASSERT_EQ(extraction.graph.edges.size(), static_cast<size_t>(2));
}

TEST(join_graph_isolates_single_relation_local_filters) {
    Catalog catalog = Catalog::with_test_tables();
    LogicalPlan plan = logical_plan_for(
        "SELECT c.name FROM customers c JOIN orders o ON c.id = o.customer_id "
        "WHERE c.country = 'US' AND o.total > 100",
        catalog);

    JoinGraphExtraction extraction = build_join_graph(find_join_search_root(plan));
    auto c_id = extraction.graph.find_relation("c");
    auto o_id = extraction.graph.find_relation("o");
    ASSERT_TRUE(c_id.has_value());
    ASSERT_TRUE(o_id.has_value());

    ASSERT_EQ(extraction.graph.relations[*c_id].local_filters.size(), static_cast<size_t>(1));
    ASSERT_EQ(extraction.graph.relations[*o_id].local_filters.size(), static_cast<size_t>(1));
    ASSERT_TRUE(extraction.residual_filters.empty());
}

// Reproduces the case where Part 1's predicate pushdown only manages to
// push a WHERE-clause filter down to wrap a *subtree* (an inner join),
// not the specific base Scan it actually belongs to (this happens because
// push_predicates doesn't recursively re-split filters it has already
// pushed down one level). build_join_graph must still attribute each
// predicate to the right single relation by re-checking which columns it
// actually references, not by its structural position in the tree.
TEST(join_graph_recovers_local_filters_even_when_pushdown_left_them_on_a_subtree) {
    Catalog catalog = Catalog::with_test_tables();
    LogicalPlan plan = logical_plan_for(
        "SELECT c.name FROM customers c "
        "JOIN orders o ON c.id = o.customer_id "
        "JOIN products p ON o.product_id = p.id "
        "WHERE c.country = 'US' AND o.total > 100",
        catalog);

    JoinGraphExtraction extraction = build_join_graph(find_join_search_root(plan));
    auto c_id = extraction.graph.find_relation("c");
    auto o_id = extraction.graph.find_relation("o");
    auto p_id = extraction.graph.find_relation("p");
    ASSERT_TRUE(c_id.has_value() && o_id.has_value() && p_id.has_value());

    ASSERT_EQ_MSG(extraction.graph.relations[*c_id].local_filters.size(), static_cast<size_t>(1),
                  "country filter should attribute to customers");
    ASSERT_EQ_MSG(
        extraction.graph.relations[*o_id].local_filters.size(), static_cast<size_t>(1),
        "total filter should attribute to orders, even though pushdown only wrapped the customers-orders subtree");
    ASSERT_TRUE(extraction.graph.relations[*p_id].local_filters.empty());
}

TEST(join_graph_edge_predicate_spanning_two_relations_becomes_an_edge) {
    Catalog catalog = Catalog::with_test_tables();
    // A WHERE-clause predicate that spans exactly two relations (rather
    // than being written as the JOIN's own ON clause) should still become
    // a graph edge -- see the pre-existing cross_table_predicate_stays_on_join
    // behavior in optimizer_tests.cpp for why this shape exists.
    LogicalPlan plan =
        logical_plan_for("SELECT c.name FROM customers c JOIN orders o ON TRUE WHERE c.id = o.customer_id", catalog);

    JoinGraphExtraction extraction = build_join_graph(find_join_search_root(plan));
    ASSERT_TRUE_MSG(extraction.graph.edges.size() >= 1, "cross-relation WHERE predicate should produce a join edge");
}

TEST(is_join_search_candidate_rejects_outer_joins) {
    Catalog catalog = Catalog::with_test_tables();
    LogicalPlan plan = logical_plan_for("SELECT * FROM customers c LEFT JOIN orders o ON c.id = o.customer_id", catalog);
    ASSERT_FALSE(is_join_search_candidate(plan));
}

TEST(is_join_search_candidate_rejects_bare_scan) {
    LogicalPlan scan = LogicalPlan::make_scan("customers", "c");
    ASSERT_FALSE(is_join_search_candidate(scan));
}

TEST(is_join_search_candidate_accepts_filter_wrapped_inner_joins) {
    Catalog catalog = Catalog::with_test_tables();
    LogicalPlan plan = logical_plan_for(
        "SELECT c.name FROM customers c JOIN orders o ON c.id = o.customer_id WHERE c.country = 'US'", catalog);
    ASSERT_TRUE(is_join_search_candidate(find_join_search_root(plan)));
}

// ═════════════════════════════════════════════════════════════════════════════
// Milestone 3.2 — Access path generator
// ═════════════════════════════════════════════════════════════════════════════

TEST(access_path_picks_index_scan_for_selective_indexed_equality) {
    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();
    CardinalityEstimator cardinality(stats);
    AccessPathGenerator gen(catalog, stats, cardinality);

    Relation rel{0, "orders", std::string("o"), {cmp(col("o", "customer_id"), BinaryOperator::Eq, int_lit(5))}};
    PhysicalPlan plan = gen.best_access_path(rel);

    ASSERT_EQ_MSG(plan.kind, PhysicalPlan::Kind::IndexScan, "customer_id is indexed and selective -- IndexScan should win");
    ASSERT_EQ(plan.index_column, std::string("customer_id"));
}

TEST(access_path_applies_multiple_local_filters) {
    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();
    CardinalityEstimator cardinality(stats);
    AccessPathGenerator gen(catalog, stats, cardinality);

    // One indexed equality (customer_id) plus one non-indexed filter
    // (status) -- both must end up applied somewhere in the resulting plan.
    Relation rel{0,
                 "orders",
                 std::string("o"),
                 {cmp(col("o", "customer_id"), BinaryOperator::Eq, int_lit(5)),
                  cmp(col("o", "status"), BinaryOperator::Eq, str_lit("shipped"))}};
    PhysicalPlan plan = gen.best_access_path(rel);

    bool has_index_scan = false;
    bool has_status_filter = false;
    const PhysicalPlan* cur = &plan;
    for (;;) {
        if (cur->kind == PhysicalPlan::Kind::IndexScan) has_index_scan = true;
        if (cur->kind == PhysicalPlan::Kind::Filter) has_status_filter = true;
        if ((cur->kind == PhysicalPlan::Kind::Filter) && cur->input) {
            cur = cur->input.get();
        } else {
            break;
        }
    }
    ASSERT_TRUE_MSG(has_index_scan, "expected the indexed equality to become an IndexScan");
    ASSERT_TRUE_MSG(has_status_filter, "expected the non-indexed filter to still be applied");
}

TEST(access_path_seq_scan_when_no_indexed_filter) {
    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();
    CardinalityEstimator cardinality(stats);
    AccessPathGenerator gen(catalog, stats, cardinality);

    Relation rel{0, "orders", std::string("o"), {cmp(col("o", "status"), BinaryOperator::Eq, str_lit("shipped"))}};
    PhysicalPlan plan = gen.best_access_path(rel);

    ASSERT_EQ_MSG(plan.kind, PhysicalPlan::Kind::Filter, "no index on status -- expect Filter over SeqScan");
    ASSERT_EQ(plan.input->kind, PhysicalPlan::Kind::SeqScan);
}

// ═════════════════════════════════════════════════════════════════════════════
// Milestones 3.3 / 3.4 / 3.5 — DP search, left-deep enumeration, connectivity
// ═════════════════════════════════════════════════════════════════════════════

TEST(join_enumerator_covers_every_relation_exactly_once) {
    Catalog catalog = Catalog::with_test_tables();
    LogicalPlan plan = logical_plan_for(
        "SELECT c.name FROM customers c "
        "JOIN orders o ON c.id = o.customer_id "
        "JOIN products p ON o.product_id = p.id",
        catalog);

    JoinGraphExtraction extraction = build_join_graph(find_join_search_root(plan));
    StatisticsCatalog stats = test_stats_catalog();
    CardinalityEstimator cardinality(stats);
    JoinEnumerator enumerator(catalog, stats, cardinality);

    PhysicalPlan best = enumerator.find_best_plan(extraction.graph);

    std::vector<std::string> leaves;
    collect_leaf_tables(best, leaves);
    std::sort(leaves.begin(), leaves.end());

    std::vector<std::string> expected = {"customers", "orders", "products"};
    ASSERT_TRUE_MSG(leaves == expected, "every relation should appear exactly once as a leaf of the winning plan");
}

TEST(join_enumerator_rejects_cartesian_product) {
    JoinGraph graph;
    graph.relations.push_back(Relation{0, "customers", std::nullopt, {}});
    graph.relations.push_back(Relation{1, "products", std::nullopt, {}});
    // No edge between them at all.

    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();
    CardinalityEstimator cardinality(stats);
    JoinEnumerator enumerator(catalog, stats, cardinality);

    bool threw = false;
    try {
        enumerator.find_best_plan(graph);
    } catch (const std::exception&) {
        threw = true;
    }
    ASSERT_TRUE_MSG(threw, "disconnected relations should refuse to produce a cartesian-product plan");
}

TEST(join_enumerator_two_relations_picks_cheapest_algorithm) {
    JoinGraph graph;
    graph.relations.push_back(Relation{0, "customers", std::string("c"), {}});
    graph.relations.push_back(Relation{1, "orders", std::string("o"), {}});
    graph.edges.push_back(JoinEdge{0, 1, cmp(col("c", "id"), BinaryOperator::Eq, col("o", "customer_id"))});

    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();
    CardinalityEstimator cardinality(stats);
    JoinEnumerator enumerator(catalog, stats, cardinality);

    PhysicalPlan best = enumerator.find_best_plan(graph);

    // customers (10,000) x orders (500,000), both unfiltered: HashJoin's
    // single pass over each side beats both NestedLoopJoin (re-scans one
    // side per row of the other) and IndexNestedLoopJoin (10,000 outer
    // rows x random-I/O-per-probe outweighs one sequential scan of
    // orders) -- matching the same result physical_tests.cpp's
    // large_equi_join_picks_hash_join already established for this exact
    // pair of tables.
    ASSERT_EQ_MSG(best.kind, PhysicalPlan::Kind::HashJoin,
                  "expected HashJoin to beat both nested-loop variants for two large, unfiltered inputs");
}

// ═════════════════════════════════════════════════════════════════════════════
// The key demonstration: chosen join order differs from SQL syntax order
// ═════════════════════════════════════════════════════════════════════════════

TEST(join_order_differs_from_sql_syntax_order_when_cheaper) {
    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();

    // Written as customers -> orders -> products, but products.category is
    // highly selective (1/8) while orders has no filter at all -- the
    // optimizer should discover that joining orders directly to products
    // first (using the index on orders.product_id) is far cheaper than
    // following the SQL order.
    std::string sql =
        "SELECT c.name FROM customers c "
        "JOIN orders o ON c.id = o.customer_id "
        "JOIN products p ON o.product_id = p.id "
        "WHERE c.country = 'US' AND p.category = 'electronics'";

    PhysicalPlan plan = physical_plan_for(sql, catalog, stats);

    ASSERT_TRUE_MSG(joined_directly(plan, "orders", "products"),
                     "expected orders and products to be joined directly to each other");

    std::vector<std::string> leaves;
    collect_leaf_tables(plan, leaves);
    ASSERT_TRUE_MSG(!leaves.empty() && leaves.front() != "customers",
                    "chosen scan order should not blindly start with the table SQL listed first");
}

TEST(full_pipeline_three_table_join_produces_a_connected_plan) {
    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();

    std::string sql =
        "SELECT c.name, o.total, p.name "
        "FROM customers c "
        "JOIN orders o ON c.id = o.customer_id "
        "JOIN products p ON o.product_id = p.id";

    PhysicalPlan plan = physical_plan_for(sql, catalog, stats);

    std::vector<std::string> leaves;
    collect_leaf_tables(plan, leaves);
    std::sort(leaves.begin(), leaves.end());
    std::vector<std::string> expected = {"customers", "orders", "products"};
    ASSERT_TRUE(leaves == expected);
}
