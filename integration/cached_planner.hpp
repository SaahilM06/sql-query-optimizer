#pragma once

#include <string>

#include "../logical/schema.hpp"
#include "../physical/physical_plan.hpp"
#include "../statistics/statistics_catalog.hpp"
#include "cache_client.hpp"
#include "cache_key.hpp"

namespace sql::integration {

struct CachedPlanResult {
    sql::physical::PhysicalPlan plan;
    bool cache_hit = false;

    // Per-phase timing, for callers that want to report/log it (see
    // metrics::QueryMetrics). plan_ms and cache_store_ms are 0.0 on a cache
    // hit -- nothing was (re)planned or (re)stored.
    double parse_ms = 0.0;
    double cache_lookup_ms = 0.0;
    double plan_ms = 0.0;
    double cache_store_ms = 0.0;
};

// Parses `sql`, builds its versioned cache key, and checks `cache` before
// running the optimizer:
//
//   hit:  deserialize the cached plan, skip planning entirely.
//   miss: run the normal pipeline (LogicalPlanner -> optimize ->
//         generate_physical_plan), then store the serialized result back
//         into `cache` with `ttl_seconds` before returning it.
//
// A cache that failed to connect (or drops mid-call) behaves exactly like a
// miss -- CacheClient::get/set already report failure that way, so this
// function has no separate "no cache" branch to get wrong. Throws
// std::runtime_error only for what the underlying pipeline itself would
// throw for (a genuinely malformed query) -- cache trouble never surfaces
// as an exception here.
CachedPlanResult plan_with_cache(const std::string& sql, const sql::logical::Catalog& schema_catalog,
                                  const sql::statistics::StatisticsCatalog& stats_catalog, CacheClient& cache,
                                  CacheVersions versions, int ttl_seconds = 300);

} // namespace sql::integration
