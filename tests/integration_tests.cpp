// Tests for the C++<->Go cache integration layer (integration/): JSON
// round-tripping, PhysicalPlan serialization, cache-key determinism, and the
// RESP wire-format pure functions used by CacheClient. Nothing here touches
// a real socket -- CacheClient's actual TCP behavior is verified manually
// against a live cache/cmd/server process (see the plan's "Manual
// end-to-end verification" section), not in this automated suite.

#include <cmath>
#include <string>

#include "../integration/cache_client.hpp"
#include "../integration/cache_key.hpp"
#include "../integration/plan_serializer.hpp"
#include "../logical/optimizer.hpp"
#include "../logical/planner.hpp"
#include "../logical/schema.hpp"
#include "../parser/ast.hpp"
#include "../parser/lexer.hpp"
#include "../parser/parser.hpp"
#include "../physical/physical_planner.hpp"
#include "../statistics/statistics_loader.hpp"
#include "../util/json.hpp"
#include "test_framework.hpp"

using namespace sql::parser;
using namespace sql::logical;
using namespace sql::physical;
using namespace sql::statistics;
using namespace sql::integration;
using sql::util::JsonValue;

namespace {

bool approx(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }

StatisticsCatalog test_stats_catalog() { return load_catalog_from_directory(SQL_OPTIMIZER_STATS_DIR); }

PhysicalPlan physical_plan_for(const std::string& sql, const Catalog& catalog, const StatisticsCatalog& stats) {
    Lexer lexer(sql);
    Parser parser(lexer.tokenize());
    Statement stmt = parser.parse();

    LogicalPlanner planner(catalog);
    LogicalPlan logical = planner.plan(std::move(stmt.select));
    LogicalPlan optimized = optimize(std::move(logical), catalog);

    return generate_physical_plan(optimized, catalog, stats);
}

Statement parse(const std::string& sql) {
    Lexer lexer(sql);
    Parser parser(lexer.tokenize());
    return parser.parse();
}

bool exprs_equal(const Expression& a, const Expression& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case Expression::Kind::Column:
            return a.table == b.table && a.column == b.column;
        case Expression::Kind::Literal:
            if (a.literal.kind != b.literal.kind) return false;
            switch (a.literal.kind) {
                case Literal::Kind::Integer: return a.literal.int_val == b.literal.int_val;
                case Literal::Kind::Float: return approx(a.literal.float_val, b.literal.float_val);
                case Literal::Kind::Str: return a.literal.str_val == b.literal.str_val;
                case Literal::Kind::Boolean: return a.literal.bool_val == b.literal.bool_val;
                case Literal::Kind::Null: return true;
            }
            return false;
        case Expression::Kind::BinaryOp:
            return a.binary_op == b.binary_op && exprs_equal(*a.left, *b.left) && exprs_equal(*a.right, *b.right);
        case Expression::Kind::UnaryOp:
            return a.unary_op == b.unary_op && exprs_equal(*a.operand, *b.operand);
        case Expression::Kind::Function:
            if (a.func_name != b.func_name || a.args.size() != b.args.size()) return false;
            for (size_t i = 0; i < a.args.size(); ++i) {
                if (!exprs_equal(a.args[i], b.args[i])) return false;
            }
            return true;
        case Expression::Kind::Wildcard:
            return true;
    }
    return false;
}

bool plans_equal(const PhysicalPlan& a, const PhysicalPlan& b) {
    if (a.kind != b.kind) return false;
    if (!approx(a.estimated_cost.cpu, b.estimated_cost.cpu)) return false;
    if (!approx(a.estimated_cost.io, b.estimated_cost.io)) return false;
    if (!approx(a.estimated_cost.memory, b.estimated_cost.memory)) return false;
    if (a.estimated_rows != b.estimated_rows) return false;
    if (a.cardinality_reasoning != b.cardinality_reasoning) return false;
    if (!approx(a.cardinality_confidence, b.cardinality_confidence)) return false;
    if (a.table_name != b.table_name) return false;
    if (a.alias != b.alias) return false;
    if (a.projected_columns != b.projected_columns) return false;
    if (a.index_column != b.index_column) return false;
    if (!exprs_equal(a.index_probe_value, b.index_probe_value)) return false;
    if (!exprs_equal(a.predicate, b.predicate)) return false;
    if ((a.input == nullptr) != (b.input == nullptr)) return false;
    if (a.input && !plans_equal(*a.input, *b.input)) return false;
    if (a.join_type != b.join_type) return false;
    if (!exprs_equal(a.condition, b.condition)) return false;
    if ((a.left == nullptr) != (b.left == nullptr)) return false;
    if (a.left && !plans_equal(*a.left, *b.left)) return false;
    if ((a.right == nullptr) != (b.right == nullptr)) return false;
    if (a.right && !plans_equal(*a.right, *b.right)) return false;
    if (a.group_by.size() != b.group_by.size()) return false;
    for (size_t i = 0; i < a.group_by.size(); ++i) {
        if (!exprs_equal(a.group_by[i], b.group_by[i])) return false;
    }
    if (a.aggregates.size() != b.aggregates.size()) return false;
    for (size_t i = 0; i < a.aggregates.size(); ++i) {
        if (a.aggregates[i].func != b.aggregates[i].func) return false;
        if (!exprs_equal(a.aggregates[i].arg, b.aggregates[i].arg)) return false;
        if (a.aggregates[i].alias != b.aggregates[i].alias) return false;
    }
    if (a.expressions.size() != b.expressions.size()) return false;
    for (size_t i = 0; i < a.expressions.size(); ++i) {
        if (!exprs_equal(a.expressions[i].first, b.expressions[i].first)) return false;
        if (a.expressions[i].second != b.expressions[i].second) return false;
    }
    if (a.order_by.size() != b.order_by.size()) return false;
    for (size_t i = 0; i < a.order_by.size(); ++i) {
        if (!exprs_equal(a.order_by[i].expression, b.order_by[i].expression)) return false;
        if (a.order_by[i].ascending != b.order_by[i].ascending) return false;
    }
    if (a.count != b.count) return false;
    return true;
}

} // namespace

// ── JSON round-trip ───────────────────────────────────────────────────────────

TEST(json_round_trip_all_kinds) {
    JsonValue obj = JsonValue::make_object();
    obj.object_val["n"] = JsonValue::make_null();
    obj.object_val["b"] = JsonValue::make_bool(true);
    obj.object_val["num"] = JsonValue::make_number(3.5);
    obj.object_val["neg"] = JsonValue::make_number(-42.0);
    obj.object_val["s"] = JsonValue::make_string("hello \"world\"\n\\backslash");
    JsonValue arr = JsonValue::make_array();
    arr.array_val.push_back(JsonValue::make_number(1));
    arr.array_val.push_back(JsonValue::make_number(2));
    obj.object_val["arr"] = std::move(arr);

    std::string text = sql::util::to_json(obj);
    JsonValue parsed = sql::util::parse_json(text);

    ASSERT_TRUE(parsed.find("n") != nullptr && parsed.find("n")->kind == JsonValue::Kind::Null);
    ASSERT_TRUE(parsed.find("b")->as_bool() == true);
    ASSERT_TRUE(approx(parsed.find("num")->as_number(), 3.5));
    ASSERT_TRUE(approx(parsed.find("neg")->as_number(), -42.0));
    ASSERT_EQ(parsed.find("s")->as_string(), std::string("hello \"world\"\n\\backslash"));
    ASSERT_EQ(parsed.find("arr")->array_val.size(), size_t(2));
}

// ── Plan serialization round-trip ─────────────────────────────────────────────

TEST(plan_round_trip_simple_scan_filter) {
    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();

    PhysicalPlan plan =
        physical_plan_for("SELECT c.name, c.email FROM customers c WHERE c.country = 'US'", catalog, stats);

    std::string json = serialize_plan(plan);
    PhysicalPlan round_tripped = deserialize_plan(json);

    ASSERT_TRUE_MSG(plans_equal(plan, round_tripped), "serialize/deserialize should reproduce the plan exactly");
}

TEST(plan_round_trip_join_order_search) {
    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();

    PhysicalPlan plan = physical_plan_for(
        "SELECT c.name FROM customers c "
        "JOIN orders o ON c.id = o.customer_id "
        "JOIN products p ON o.product_id = p.id "
        "WHERE c.country = 'US' AND p.category = 'electronics'",
        catalog, stats);

    std::string json = serialize_plan(plan);
    PhysicalPlan round_tripped = deserialize_plan(json);

    ASSERT_TRUE_MSG(plans_equal(plan, round_tripped), "join-order-search plan should round-trip exactly");
}

TEST(plan_round_trip_aggregate_sort_limit) {
    Catalog catalog = Catalog::with_test_tables();
    StatisticsCatalog stats = test_stats_catalog();

    PhysicalPlan plan = physical_plan_for(
        "SELECT o.id, SUM(o.total) FROM orders o "
        "INNER JOIN customers c ON o.customer_id = c.id "
        "WHERE c.country = 'US' "
        "GROUP BY o.id "
        "HAVING SUM(o.total) > 100 "
        "ORDER BY o.id DESC "
        "LIMIT 20",
        catalog, stats);

    std::string json = serialize_plan(plan);
    PhysicalPlan round_tripped = deserialize_plan(json);

    ASSERT_TRUE_MSG(plans_equal(plan, round_tripped), "aggregate/sort/limit stack should round-trip exactly");
}

// ── Cache key determinism ─────────────────────────────────────────────────────

TEST(cache_key_ignores_whitespace_and_keyword_case) {
    Catalog catalog = Catalog::with_test_tables();
    CacheVersions versions = compute_versions(catalog, SQL_OPTIMIZER_STATS_DIR);

    Statement a = parse("SELECT * FROM orders");
    Statement b = parse("select   *   from   orders");

    ASSERT_EQ(build_cache_key(a, versions), build_cache_key(b, versions));
}

TEST(cache_key_differs_for_different_predicate) {
    Catalog catalog = Catalog::with_test_tables();
    CacheVersions versions = compute_versions(catalog, SQL_OPTIMIZER_STATS_DIR);

    Statement a = parse("SELECT * FROM orders WHERE id = 1");
    Statement b = parse("SELECT * FROM orders WHERE id = 2");

    ASSERT_TRUE(build_cache_key(a, versions) != build_cache_key(b, versions));
}

TEST(cache_key_differs_for_different_versions) {
    Catalog catalog = Catalog::with_test_tables();
    Statement stmt = parse("SELECT * FROM orders");

    CacheVersions v1{1, 1};
    CacheVersions v2{2, 1};
    CacheVersions v3{1, 2};

    std::string k1 = build_cache_key(stmt, v1);
    std::string k2 = build_cache_key(stmt, v2);
    std::string k3 = build_cache_key(stmt, v3);

    ASSERT_TRUE(k1 != k2);
    ASSERT_TRUE(k1 != k3);
    ASSERT_TRUE(k2 != k3);
}

TEST(cache_key_matches_documented_format) {
    Catalog catalog = Catalog::with_test_tables();
    Statement stmt = parse("SELECT * FROM orders");
    CacheVersions versions{7, 24};

    std::string key = build_cache_key(stmt, versions);

    ASSERT_TRUE_MSG(key.rfind("plan:", 0) == 0, "key should start with the documented 'plan:' prefix");
    ASSERT_TRUE_MSG(key.find(":7:24") != std::string::npos, "key should end with the schema/stats version segments");
}

// ── RESP wire format (pure functions, no socket) ──────────────────────────────

TEST(resp_encode_command_matches_wire_format) {
    std::string encoded = encode_command({"GET", "foo"});
    ASSERT_EQ(encoded, std::string("*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n"));
}

TEST(resp_parse_reply_simple_string) {
    StringByteReader reader("+OK\r\n");
    Reply r = parse_reply(reader);
    ASSERT_TRUE(r.type == Reply::Type::Ok);
    ASSERT_EQ(r.string_val, std::string("OK"));
}

TEST(resp_parse_reply_bulk_string) {
    StringByteReader reader("$3\r\nbar\r\n");
    Reply r = parse_reply(reader);
    ASSERT_TRUE(r.type == Reply::Type::Bulk);
    ASSERT_EQ(r.bulk_val, std::string("bar"));
}

TEST(resp_parse_reply_null_bulk) {
    StringByteReader reader("$-1\r\n");
    Reply r = parse_reply(reader);
    ASSERT_TRUE(r.is_null());
}

TEST(resp_parse_reply_error) {
    StringByteReader reader("-ERR something went wrong\r\n");
    Reply r = parse_reply(reader);
    ASSERT_TRUE(r.type == Reply::Type::Error);
    ASSERT_EQ(r.string_val, std::string("ERR something went wrong"));
}

TEST(resp_parse_reply_integer) {
    StringByteReader reader(":42\r\n");
    Reply r = parse_reply(reader);
    ASSERT_TRUE(r.type == Reply::Type::Integer);
    ASSERT_EQ(r.integer_val, int64_t(42));
}
