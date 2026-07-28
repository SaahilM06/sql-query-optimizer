// Tests for the statistics/cardinality/selectivity subsystem (Part 2):
// StatisticsCatalog, SelectivityEstimator, CardinalityEstimator, and the
// histogram-driven range estimates. Uses tiny, hand-built statistics
// (matching the spec's own worked examples) rather than the full
// stats/*.json fixtures, except where explicitly noted.

#include <cmath>
#include <string>

#include "../src/logical/logical_plan.hpp"
#include "../src/optimizer/cardinality_estimator.hpp"
#include "../src/optimizer/selectivity_estimator.hpp"
#include "../src/parser/ast.hpp"
#include "../src/statistics/statistics_catalog.hpp"
#include "../src/statistics/statistics_loader.hpp"
#include "test_framework.hpp"

using namespace sql::parser;
using namespace sql::logical;
using namespace sql::statistics;
using namespace sql::optimizer;

namespace {

bool approx(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }

Expression col(const std::string& name) { return Expression::make_column(std::nullopt, name); }
Expression qcol(const std::string& table, const std::string& name) { return Expression::make_column(table, name); }
Expression int_lit(int64_t v) { return Expression::make_literal(Literal::integer(v)); }
Expression float_lit(double v) { return Expression::make_literal(Literal::floating(v)); }
Expression str_lit(const std::string& v) { return Expression::make_literal(Literal::str(v)); }

Expression cmp(Expression left, BinaryOperator op, Expression right) {
    return Expression::make_binary_op(std::move(left), op, std::move(right));
}

// users: rows=1,000, age distinct=100, country distinct=10, both no nulls.
TableStats users_stats() {
    TableStats t;
    t.row_count = 1000;
    ColumnStats age;
    age.distinct_count = 100;
    age.null_fraction = 0.0;
    t.columns["age"] = age;
    ColumnStats country;
    country.distinct_count = 10;
    country.null_fraction = 0.0;
    t.columns["country"] = country;
    return t;
}

} // namespace

// ── Equality selectivity (uniform NDV formula) ───────────────────────────────

TEST(equality_selectivity_uniform_distribution) {
    TableStats stats = users_stats();
    SelectivityEstimator est;

    Expression pred = cmp(col("country"), BinaryOperator::Eq, str_lit("US"));
    double sel = est.estimate(pred, stats);

    ASSERT_TRUE_MSG(approx(sel, 0.10), "expected selectivity 0.10 for 1/10 distinct values");
}

TEST(equality_selectivity_accounts_for_nulls) {
    TableStats stats;
    ColumnStats cs;
    cs.distinct_count = 10;
    cs.null_fraction = 0.5;
    stats.columns["x"] = cs;
    stats.row_count = 1000;

    SelectivityEstimator est;
    double sel = est.estimate(cmp(col("x"), BinaryOperator::Eq, int_lit(1)), stats);

    ASSERT_TRUE_MSG(approx(sel, 0.05), "expected (1-0.5)/10 = 0.05, got varying result");
}

// ── AND combination (independence assumption) ────────────────────────────────

TEST(and_combines_selectivities_by_multiplication) {
    TableStats stats = users_stats();
    SelectivityEstimator est;

    Expression pred = cmp(cmp(col("age"), BinaryOperator::Eq, int_lit(25)), BinaryOperator::And,
                           cmp(col("country"), BinaryOperator::Eq, str_lit("US")));
    double sel = est.estimate(pred, stats);

    ASSERT_TRUE_MSG(approx(sel, 0.001), "expected 0.01 * 0.10 = 0.001");
}

TEST(or_combines_selectivities_by_inclusion_exclusion) {
    TableStats stats = users_stats();
    SelectivityEstimator est;

    Expression pred = cmp(cmp(col("age"), BinaryOperator::Eq, int_lit(25)), BinaryOperator::Or,
                           cmp(col("country"), BinaryOperator::Eq, str_lit("US")));
    double sel = est.estimate(pred, stats);

    double expected = 0.01 + 0.10 - (0.01 * 0.10);
    ASSERT_TRUE(approx(sel, expected));
}

TEST(not_inverts_selectivity) {
    TableStats stats = users_stats();
    SelectivityEstimator est;

    Expression pred = Expression::make_unary_op(UnaryOperator::Not, cmp(col("country"), BinaryOperator::Eq, str_lit("US")));
    double sel = est.estimate(pred, stats);

    ASSERT_TRUE(approx(sel, 0.90));
}

// ── Range selectivity (min/max uniform fallback) ─────────────────────────────

TEST(range_selectivity_uniform_min_max) {
    TableStats stats;
    ColumnStats total;
    total.min_value = 0.0;
    total.max_value = 500.0;
    total.null_fraction = 0.0;
    stats.columns["total"] = total;

    SelectivityEstimator est;
    double sel = est.estimate(cmp(col("total"), BinaryOperator::Gt, float_lit(100.0)), stats);

    ASSERT_TRUE_MSG(approx(sel, 0.80), "(500-100)/(500-0) = 0.80");
}

TEST(range_selectivity_outside_max_is_zero) {
    TableStats stats;
    ColumnStats total;
    total.min_value = 0.0;
    total.max_value = 500.0;
    stats.columns["total"] = total;

    SelectivityEstimator est;
    double sel = est.estimate(cmp(col("total"), BinaryOperator::Gt, float_lit(1000.0)), stats);

    ASSERT_TRUE_MSG(approx(sel, 0.0), "predicate value beyond max should yield 0 selectivity");
}

TEST(range_selectivity_below_min_is_full_minus_nulls) {
    TableStats stats;
    ColumnStats total;
    total.min_value = 0.0;
    total.max_value = 500.0;
    total.null_fraction = 0.1;
    stats.columns["total"] = total;

    SelectivityEstimator est;
    double sel = est.estimate(cmp(col("total"), BinaryOperator::Gt, float_lit(-10.0)), stats);

    ASSERT_TRUE_MSG(approx(sel, 0.90), "value below min should yield the full non-null fraction");
}

// ── Histogram-based range selectivity ────────────────────────────────────────

TableStats orders_total_histogram_stats() {
    TableStats stats;
    ColumnStats total;
    total.min_value = 0.0;
    total.max_value = 1000.0;
    Histogram h;
    h.buckets = {
        {0, 25, 0.40},
        {25, 50, 0.25},
        {50, 100, 0.20},
        {100, 250, 0.10},
        {250, 1000, 0.05},
    };
    total.histogram = h;
    stats.columns["total"] = total;
    return stats;
}

TEST(histogram_range_selectivity_at_bucket_boundary) {
    TableStats stats = orders_total_histogram_stats();
    SelectivityEstimator est;

    double sel = est.estimate(cmp(col("total"), BinaryOperator::Gt, float_lit(100.0)), stats);

    ASSERT_TRUE_MSG(approx(sel, 0.15), "10% + 5% = 15%, matching the spec's own worked example");
}

TEST(histogram_range_selectivity_prorates_partial_bucket_overlap) {
    TableStats stats = orders_total_histogram_stats();
    SelectivityEstimator est;

    // total > 200 overlaps the [100,250) bucket by 1/3 of its width, plus
    // the full [250,1000) bucket.
    double sel = est.estimate(cmp(col("total"), BinaryOperator::Gt, float_lit(200.0)), stats);
    double expected = 0.10 * (50.0 / 150.0) + 0.05;

    ASSERT_TRUE(approx(sel, expected, 1e-4));
}

// ── Default fallback for unresolvable predicate shapes ───────────────────────

TEST(column_to_column_comparison_uses_default_selectivity) {
    TableStats stats = users_stats();
    SelectivityEstimator est;

    double sel = est.estimate(cmp(col("age"), BinaryOperator::Eq, col("country")), stats);

    ASSERT_TRUE(approx(sel, kDefaultSelectivity));
}

TEST(missing_column_stats_uses_default_selectivity) {
    TableStats stats; // no columns registered at all
    SelectivityEstimator est;

    double sel = est.estimate(cmp(col("nonexistent"), BinaryOperator::Eq, int_lit(1)), stats);

    ASSERT_TRUE(approx(sel, kDefaultSelectivity));
}

// ── CardinalityEstimator: scans ───────────────────────────────────────────────

TEST(scan_cardinality_uses_row_count) {
    StatisticsCatalog catalog;
    TableStats t;
    t.row_count = 100000;
    catalog.register_table("customers", t);

    CardinalityEstimator est(catalog);
    Estimate e = est.estimate_scan("customers");

    ASSERT_TRUE(approx(e.rows, 100000.0));
    ASSERT_TRUE(e.confidence > 0.9);
}

TEST(scan_cardinality_missing_table_uses_default_with_low_confidence) {
    StatisticsCatalog catalog; // empty
    CardinalityEstimator est(catalog);
    Estimate e = est.estimate_scan("unknown_table");

    ASSERT_TRUE_MSG(e.rows > 0.0, "should still produce a usable default guess");
    ASSERT_TRUE_MSG(e.confidence < 0.5, "confidence should reflect the missing statistics");
}

TEST(scan_cardinality_zero_row_table) {
    StatisticsCatalog catalog;
    TableStats t;
    t.row_count = 0;
    catalog.register_table("empty_table", t);

    CardinalityEstimator est(catalog);
    Estimate e = est.estimate_scan("empty_table");

    ASSERT_TRUE(approx(e.rows, 0.0));
    ASSERT_TRUE_MSG(e.confidence > 0.9, "a real (if zero) statistic should still be high-confidence");
}

// ── CardinalityEstimator: filters ─────────────────────────────────────────────

TEST(filter_cardinality_resolves_single_table_scope) {
    StatisticsCatalog catalog;
    catalog.register_table("users", users_stats());

    CardinalityEstimator est(catalog);
    LogicalPlan scan = LogicalPlan::make_scan("users", std::nullopt);
    Expression pred = cmp(col("country"), BinaryOperator::Eq, str_lit("US"));

    Estimate e = est.estimate_filter(1000.0, pred, scan);

    ASSERT_TRUE_MSG(approx(e.rows, 100.0), "1000 rows * 0.10 selectivity = 100 rows");
}

TEST(filter_cardinality_impossible_predicate_floors_at_one_row) {
    StatisticsCatalog catalog;
    TableStats stats;
    stats.row_count = 1000;
    ColumnStats total;
    total.min_value = 0.0;
    total.max_value = 500.0;
    stats.columns["total"] = total;
    catalog.register_table("orders", stats);

    CardinalityEstimator est(catalog);
    LogicalPlan scan = LogicalPlan::make_scan("orders", std::nullopt);
    Expression pred = cmp(col("total"), BinaryOperator::Gt, float_lit(10000.0)); // impossible

    Estimate e = est.estimate_filter(1000.0, pred, scan);

    ASSERT_TRUE_MSG(approx(e.rows, 1.0), "cardinality never estimates exactly zero rows");
}

TEST(filter_cardinality_multi_relation_scope_falls_back_to_heuristic) {
    StatisticsCatalog catalog;
    CardinalityEstimator est(catalog);

    LogicalPlan left = LogicalPlan::make_scan("a", std::nullopt);
    LogicalPlan right = LogicalPlan::make_scan("b", std::nullopt);
    LogicalPlan join = LogicalPlan::make_join(JoinType::Inner, cmp(qcol("a", "x"), BinaryOperator::Eq, qcol("b", "y")),
                                               std::move(left), std::move(right));

    Estimate e = est.estimate_filter(1000.0, cmp(col("z"), BinaryOperator::Eq, int_lit(1)), join);

    ASSERT_TRUE_MSG(approx(e.rows, 100.0), "expected default 10% heuristic (1000/10)");
    ASSERT_TRUE(e.confidence < 0.5);
}

// ── CardinalityEstimator: joins ───────────────────────────────────────────────

TEST(join_cardinality_ndv_formula) {
    StatisticsCatalog catalog;

    TableStats customers;
    customers.row_count = 100;
    ColumnStats id;
    id.distinct_count = 100;
    customers.columns["id"] = id;
    catalog.register_table("customers", customers);

    TableStats orders;
    orders.row_count = 1000;
    ColumnStats customer_id;
    customer_id.distinct_count = 100;
    orders.columns["customer_id"] = customer_id;
    catalog.register_table("orders", orders);

    CardinalityEstimator est(catalog);
    LogicalPlan left = LogicalPlan::make_scan("customers", "c");
    LogicalPlan right = LogicalPlan::make_scan("orders", "o");
    Expression condition = cmp(qcol("c", "id"), BinaryOperator::Eq, qcol("o", "customer_id"));

    Estimate e = est.estimate_join(100.0, 1000.0, condition, left, right);

    ASSERT_TRUE_MSG(approx(e.rows, 1000.0), "100 * 1000 / max(100,100) = 1000");
}

TEST(join_cardinality_falls_back_for_non_equi_condition) {
    StatisticsCatalog catalog;
    CardinalityEstimator est(catalog);

    LogicalPlan left = LogicalPlan::make_scan("a", std::nullopt);
    LogicalPlan right = LogicalPlan::make_scan("b", std::nullopt);
    Expression condition = cmp(col("x"), BinaryOperator::Gt, col("y"));

    Estimate e = est.estimate_join(100.0, 200.0, condition, left, right);

    ASSERT_TRUE_MSG(approx(e.rows, 2000.0), "expected default 10% of cartesian product: (100*200)/10");
}

// ── CardinalityEstimator: aggregate / project / sort / limit ────────────────

TEST(aggregate_cardinality_product_of_group_by_ndv) {
    StatisticsCatalog catalog;
    TableStats stats;
    stats.row_count = 1000;
    ColumnStats country;
    country.distinct_count = 20;
    stats.columns["country"] = country;
    catalog.register_table("customers", stats);

    CardinalityEstimator est(catalog);
    LogicalPlan scan = LogicalPlan::make_scan("customers", std::nullopt);

    std::vector<Expression> group_by;
    group_by.push_back(col("country"));

    Estimate e = est.estimate_aggregate(1000.0, group_by, scan);

    ASSERT_TRUE(approx(e.rows, 20.0));
}

TEST(aggregate_cardinality_caps_at_input_rows) {
    StatisticsCatalog catalog;
    TableStats stats;
    stats.row_count = 5;
    ColumnStats a, b;
    a.distinct_count = 100;
    b.distinct_count = 100;
    stats.columns["a"] = a;
    stats.columns["b"] = b;
    catalog.register_table("t", stats);

    CardinalityEstimator est(catalog);
    LogicalPlan scan = LogicalPlan::make_scan("t", std::nullopt);
    std::vector<Expression> group_by{col("a"), col("b")};

    Estimate e = est.estimate_aggregate(5.0, group_by, scan);

    ASSERT_TRUE_MSG(approx(e.rows, 5.0), "100*100=10000 groups should be capped at the 5-row input");
}

TEST(aggregate_no_group_by_is_one_row) {
    StatisticsCatalog catalog;
    CardinalityEstimator est(catalog);
    LogicalPlan scan = LogicalPlan::make_scan("t", std::nullopt);

    Estimate e = est.estimate_aggregate(100000.0, {}, scan);

    ASSERT_TRUE(approx(e.rows, 1.0));
}

TEST(project_and_sort_do_not_change_row_count) {
    ASSERT_TRUE(approx(CardinalityEstimator::estimate_project(500.0).rows, 500.0));
    ASSERT_TRUE(approx(CardinalityEstimator::estimate_sort(500.0).rows, 500.0));
}

TEST(limit_smaller_than_input) {
    Estimate e = CardinalityEstimator::estimate_limit(1000.0, 10);
    ASSERT_TRUE(approx(e.rows, 10.0));
}

TEST(limit_larger_than_input) {
    Estimate e = CardinalityEstimator::estimate_limit(5.0, 100);
    ASSERT_TRUE_MSG(approx(e.rows, 5.0), "LIMIT larger than the input shouldn't inflate the estimate");
}

// ── resolve_single_table ──────────────────────────────────────────────────────

TEST(resolve_single_table_through_filter_wrapper) {
    LogicalPlan scan = LogicalPlan::make_scan("orders", "o");
    LogicalPlan filter = LogicalPlan::make_filter(cmp(col("total"), BinaryOperator::Gt, int_lit(0)), std::move(scan));

    auto resolved = resolve_single_table(filter);
    ASSERT_TRUE_MSG(resolved.has_value(), "should resolve through a Filter wrapper to the underlying Scan");
    ASSERT_EQ(resolved->table_name, std::string("orders"));
    ASSERT_EQ(resolved->alias_or_name, std::string("o"));
}

TEST(resolve_single_table_fails_for_join) {
    LogicalPlan left = LogicalPlan::make_scan("a", std::nullopt);
    LogicalPlan right = LogicalPlan::make_scan("b", std::nullopt);
    LogicalPlan join = LogicalPlan::make_join(JoinType::Inner, cmp(int_lit(1), BinaryOperator::Eq, int_lit(1)),
                                               std::move(left), std::move(right));

    auto resolved = resolve_single_table(join);
    ASSERT_FALSE(resolved.has_value());
}

// ── Real stats/*.json fixtures ─────────────────────────────────────────────────

TEST(loaded_orders_json_matches_spec_worked_histogram_example) {
    TableStats stats = load_table_stats_from_file(std::string(SQL_OPTIMIZER_STATS_DIR) + "/orders.json");
    ASSERT_TRUE(approx(stats.row_count, 500000.0));

    SelectivityEstimator est;
    double sel = est.estimate(cmp(col("total"), BinaryOperator::Gt, float_lit(100.0)), stats);

    ASSERT_TRUE_MSG(approx(sel, 0.15), "orders.json's histogram should reproduce the spec's 15% worked example");
}

TEST(load_catalog_from_directory_finds_all_three_tables) {
    StatisticsCatalog catalog = load_catalog_from_directory(SQL_OPTIMIZER_STATS_DIR);

    ASSERT_TRUE(catalog.get("customers") != nullptr);
    ASSERT_TRUE(catalog.get("orders") != nullptr);
    ASSERT_TRUE(catalog.get("products") != nullptr);
    ASSERT_TRUE(catalog.get("nonexistent") == nullptr);
}
