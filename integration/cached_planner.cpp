#include "cached_planner.hpp"

#include "../logical/optimizer.hpp"
#include "../logical/planner.hpp"
#include "../parser/lexer.hpp"
#include "../parser/parser.hpp"
#include "../physical/physical_planner.hpp"
#include "plan_serializer.hpp"

namespace sql::integration {

CachedPlanResult plan_with_cache(const std::string& sql, const sql::logical::Catalog& schema_catalog,
                                  const sql::statistics::StatisticsCatalog& stats_catalog, CacheClient& cache,
                                  CacheVersions versions, int ttl_seconds) {
    sql::parser::Lexer lexer(sql);
    sql::parser::Parser parser(lexer.tokenize());
    sql::parser::Statement stmt = parser.parse();

    std::string key = build_cache_key(stmt, versions);

    if (auto cached = cache.get(key)) {
        return CachedPlanResult{deserialize_plan(*cached), /*cache_hit=*/true};
    }

    sql::logical::LogicalPlanner logical_planner(schema_catalog);
    auto logical_plan = logical_planner.plan(std::move(stmt.select));
    auto optimized = sql::logical::optimize(std::move(logical_plan), schema_catalog);
    auto physical_plan = sql::physical::generate_physical_plan(optimized, schema_catalog, stats_catalog);

    cache.set(key, serialize_plan(physical_plan), ttl_seconds);

    return CachedPlanResult{std::move(physical_plan), /*cache_hit=*/false};
}

} // namespace sql::integration
