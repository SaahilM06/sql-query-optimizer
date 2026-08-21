#pragma once

#include <string>
#include <vector>

#include "../integration/cache_client.hpp"
#include "../integration/cache_key.hpp"
#include "../logical/schema.hpp"
#include "../statistics/statistics_catalog.hpp"
#include "../storage/database.hpp"
#include "../storage/row.hpp"

namespace sql::distributed {

struct WorkerAddress {
    std::string host;
    int port;
};

struct DistributedQueryResult {
    std::vector<std::string> columns;
    std::vector<sql::storage::Row> rows;
    bool distributed = false;    // false if this fell back to single-node execution
    std::string fallback_reason; // populated iff !distributed
    double total_ms = 0.0;
    size_t workers_used = 0;
};

// Plans `sql` (unchanged single-node planning -- join order/algorithm
// selection isn't touched by any of this), then either distributes its
// execution across `workers` or falls back to running it locally against
// `local_full_database`, reported via fallback_reason rather than
// attempted unsoundly.
//
// Eligible for distribution: the plan's outermost join, when both sides
// are a single base relation (broadcast the smaller side if it's under a
// threshold, otherwise shuffle both sides by the join key), or a top-level
// GROUP BY with no join at all (distributed partial-aggregate + coordinator
// merge). Anything else -- most importantly a 3+ table join, which would
// need an exchange inserted at more than one point in the tree -- falls
// back. See coordinator.cpp's top comment for exactly why that's the
// boundary for this build.
DistributedQueryResult run_distributed_query(const std::string& sql, const sql::logical::Catalog& schema_catalog,
                                              const sql::statistics::StatisticsCatalog& stats_catalog,
                                              const sql::storage::Database& local_full_database,
                                              const std::vector<WorkerAddress>& workers,
                                              sql::integration::CacheClient& cache,
                                              sql::integration::CacheVersions versions);

} // namespace sql::distributed
