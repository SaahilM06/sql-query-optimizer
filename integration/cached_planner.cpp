#include "cached_planner.hpp"

#include <chrono>

#include "../logical/optimizer.hpp"
#include "../logical/planner.hpp"
#include "../parser/lexer.hpp"
#include "../parser/parser.hpp"
#include "../physical/physical_planner.hpp"
#include "plan_serializer.hpp"

namespace sql::integration {

namespace {
double ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
} // namespace

CachedPlanResult plan_with_cache(const std::string& sql, const sql::logical::Catalog& schema_catalog,
                                  const sql::statistics::StatisticsCatalog& stats_catalog, CacheClient& cache,
                                  CacheVersions versions, int ttl_seconds) {
    CachedPlanResult result;

    auto t_parse = std::chrono::steady_clock::now();
    sql::parser::Lexer lexer(sql);
    sql::parser::Parser parser(lexer.tokenize());
    sql::parser::Statement stmt = parser.parse();
    result.parse_ms = ms_since(t_parse);

    std::string key = build_cache_key(stmt, versions);

    auto t_lookup = std::chrono::steady_clock::now();
    auto cached = cache.get(key);
    result.cache_lookup_ms = ms_since(t_lookup);

    if (cached) {
        result.plan = deserialize_plan(*cached);
        result.cache_hit = true;
        return result;
    }

    auto t_plan = std::chrono::steady_clock::now();
    sql::logical::LogicalPlanner logical_planner(schema_catalog);
    auto logical_plan = logical_planner.plan(std::move(stmt.select));
    auto optimized = sql::logical::optimize(std::move(logical_plan), schema_catalog);
    auto physical_plan = sql::physical::generate_physical_plan(optimized, schema_catalog, stats_catalog);
    result.plan_ms = ms_since(t_plan);

    auto t_store = std::chrono::steady_clock::now();
    cache.set(key, serialize_plan(physical_plan), ttl_seconds);
    result.cache_store_ms = ms_since(t_store);

    result.plan = std::move(physical_plan);
    result.cache_hit = false;
    return result;
}

} // namespace sql::integration
