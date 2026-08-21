#include <array>
#include <cstdlib>
#include <iostream>

#include "integration/cache_client.hpp"
#include "integration/cache_key.hpp"
#include "integration/cached_planner.hpp"
#include "logical/optimizer.hpp"
#include "logical/planner.hpp"
#include "logical/schema.hpp"
#include "optimizer/explain.hpp"
#include "parser/lexer.hpp"
#include "parser/parser.hpp"
#include "parser/sql_printer.hpp"
#include "physical/physical_planner.hpp"
#include "statistics/statistics_loader.hpp"

using namespace sql::parser;

int main() {
    const std::array<const char*, 4> queries = {
        "SELECT * FROM orders",
        "SELECT c.name, c.email FROM customers c WHERE c.country = 'US'",
        "SELECT o.id, SUM(o.total) FROM orders o "
        "INNER JOIN customers c ON o.customer_id = c.id "
        "WHERE c.active = TRUE "
        "GROUP BY o.id "
        "HAVING SUM(o.total) > 100 "
        "ORDER BY o.id DESC "
        "LIMIT 20",
        // precedence: OR(a=1, AND(b=2, c=3))
        "SELECT 1 FROM t WHERE a = 1 OR b = 2 AND c = 3",
    };

    for (const char* sql : queries) {
        std::cout << "SQL: " << sql << "\n";
        try {
            Lexer lexer(sql);
            std::vector<Token> tokens = lexer.tokenize();
            Parser parser(std::move(tokens));
            Statement ast = parser.parse();
            std::cout << "AST: " << to_canonical_sql(ast) << "\n\n";
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << "\n\n";
        }
    }

    // ── Part 2 demo: cardinality/cost-annotated physical plan ────────────────
    //
    // Same worked example from the cardinality-estimation spec: an equi-join
    // with a WHERE predicate on each side, showing where every row estimate
    // came from.
    std::cout << "──────────────────────────────────────────────────────────\n";
    std::cout << "Annotated physical plan (Part 2: cardinality + cost)\n";
    std::cout << "──────────────────────────────────────────────────────────\n";

    const char* demo_sql =
        "SELECT c.name "
        "FROM customers c "
        "JOIN orders o ON c.id = o.customer_id "
        "WHERE c.country = 'US' AND o.total > 100";
    std::cout << "SQL: " << demo_sql << "\n\n";

    try {
        auto schema_catalog = sql::logical::Catalog::with_test_tables();
        auto stats_catalog = sql::statistics::load_catalog_from_directory(SQL_OPTIMIZER_STATS_DIR);

        Lexer lexer(demo_sql);
        Parser parser(lexer.tokenize());
        Statement stmt = parser.parse();

        sql::logical::LogicalPlanner logical_planner(schema_catalog);
        auto logical_plan = logical_planner.plan(std::move(stmt.select));
        auto optimized = sql::logical::optimize(std::move(logical_plan), schema_catalog);

        auto physical_plan = sql::physical::generate_physical_plan(optimized, schema_catalog, stats_catalog);

        std::cout << "Chosen top-level strategy: "
                   << (physical_plan.kind == sql::physical::PhysicalPlan::Kind::Project ? "Project" : "other") << "\n\n";
        sql::optimizer::explain_plan(physical_plan, std::cout);
    } catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << "\n";
    }

    // ── Part 3 demo: cost-based join order search ────────────────────────────
    //
    // The spec's own worked example: three tables joined in SQL as
    // customers -> orders -> products, with a selective filter on each end
    // (customers.country and products.category) but none on orders. The DP
    // search should discover that starting with orders <-> products (the
    // more selective pair once products is filtered) beats blindly
    // following the SQL FROM/JOIN order.
    std::cout << "\n──────────────────────────────────────────────────────────\n";
    std::cout << "Cost-based join order search (Part 3)\n";
    std::cout << "──────────────────────────────────────────────────────────\n";

    const char* join_search_sql =
        "SELECT c.name "
        "FROM customers c "
        "JOIN orders o ON c.id = o.customer_id "
        "JOIN products p ON o.product_id = p.id "
        "WHERE c.country = 'US' AND p.category = 'electronics'";
    std::cout << "SQL: " << join_search_sql << "\n";
    std::cout << "(SQL join order as written: customers -> orders -> products)\n\n";

    try {
        auto schema_catalog = sql::logical::Catalog::with_test_tables();
        auto stats_catalog = sql::statistics::load_catalog_from_directory(SQL_OPTIMIZER_STATS_DIR);

        Lexer lexer(join_search_sql);
        Parser parser(lexer.tokenize());
        Statement stmt = parser.parse();

        sql::logical::LogicalPlanner logical_planner(schema_catalog);
        auto logical_plan = logical_planner.plan(std::move(stmt.select));
        auto optimized = sql::logical::optimize(std::move(logical_plan), schema_catalog);

        auto physical_plan = sql::physical::generate_physical_plan(optimized, schema_catalog, stats_catalog);

        std::cout << "Chosen physical plan:\n\n";
        sql::optimizer::explain_plan(physical_plan, std::cout);
    } catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << "\n";
    }

    // ── Part 4 demo: cache-integrated planning ────────────────────────────────
    //
    // Runs the same join-search query above through plan_with_cache twice.
    // With cache/cmd/server running (see cache/README.md), the first call
    // computes the plan and stores it; the second finds it already cached
    // and skips re-planning entirely. With no cache reachable, both calls
    // just fall through to the normal pipeline -- proving the optimizer
    // never depends on the cache being up.
    std::cout << "\n──────────────────────────────────────────────────────────\n";
    std::cout << "Cache-integrated planning (Part 4)\n";
    std::cout << "──────────────────────────────────────────────────────────\n";

    std::string cache_addr = "127.0.0.1:6380";
    if (const char* env = std::getenv("SQLOPT_CACHE_ADDR")) cache_addr = env;
    size_t colon = cache_addr.find(':');
    std::string cache_host = colon == std::string::npos ? cache_addr : cache_addr.substr(0, colon);
    int cache_port = colon == std::string::npos ? 6380 : std::stoi(cache_addr.substr(colon + 1));

    sql::integration::CacheClient cache(cache_host, cache_port);
    if (cache.connect()) {
        std::cout << "Connected to cache at " << cache_addr << "\n\n";
    } else {
        std::cout << "Cache unavailable at " << cache_addr
                   << " -- running standalone (expected if cache/cmd/server isn't running).\n\n";
    }

    try {
        auto schema_catalog = sql::logical::Catalog::with_test_tables();
        auto stats_catalog = sql::statistics::load_catalog_from_directory(SQL_OPTIMIZER_STATS_DIR);
        auto versions = sql::integration::compute_versions(schema_catalog, SQL_OPTIMIZER_STATS_DIR);

        std::cout << "SQL: " << join_search_sql << "\n\n";

        for (int run = 1; run <= 2; ++run) {
            auto result = sql::integration::plan_with_cache(join_search_sql, schema_catalog, stats_catalog, cache, versions);
            std::cout << "Run " << run << ": "
                       << (result.cache_hit ? "cache hit (skipped re-optimization)" : "cache miss (computed and stored)")
                       << "\n";
        }
        std::cout << "\n";
    } catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << "\n";
    }

    return 0;
}
