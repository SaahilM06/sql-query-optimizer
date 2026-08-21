#pragma once

#include <string>
#include <vector>

#include "../adaptive/bandit.hpp"
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

    // "broadcast_left" | "broadcast_right" | "shuffle" | "single" | "" (fallback)
    std::string join_strategy;
    // True if the shuffle+aggregate path recognized the GROUP BY key
    // matches the shuffle's partition key and skipped the cross-worker
    // merge entirely (see distributed/properties.hpp) -- each worker's
    // partial aggregate was already the complete, final answer for every
    // group it produced.
    bool used_copartition_merge_skip = false;

    // Indices (into the `workers` vector passed to run_distributed_query)
    // of any worker that didn't respond during this query -- its
    // contribution was recomputed by the coordinator itself instead (see
    // coordinator.cpp's call_worker_or_recover). Empty means every worker
    // that was contacted actually answered. The query still completes
    // correctly either way; this is purely for visibility into whether
    // recovery happened.
    std::vector<size_t> recovered_workers;
};

// Plans `sql` (unchanged single-node planning -- join order/algorithm
// selection isn't touched by any of this), then either distributes its
// execution across `workers` or falls back to running it locally against
// `local_full_database`, reported via fallback_reason rather than
// attempted unsoundly.
//
// Eligible for distribution: the plan's outermost join, when both sides
// are a single base relation (bandit-chosen broadcast vs. shuffle when
// broadcast is legal at all, i.e. at least one side is under a size
// ceiling -- see coordinator.cpp; shuffle only when neither side
// qualifies), or a top-level GROUP BY with no join at all (distributed
// partial-aggregate + coordinator merge). Anything else -- most
// importantly a 3+ table join, which would need an exchange inserted at
// more than one point in the tree -- falls back. See coordinator.cpp's top
// comment for exactly why that's the boundary for this build.
//
// `bandit` is read and updated in place: it decides broadcast-vs-shuffle
// when both are legal, and is trained on this call's observed total_ms
// before returning, so its policy improves across calls within a session
// (and across sessions, if the caller persists it -- see
// adaptive/bandit.hpp's load_bandit/save_bandit).
DistributedQueryResult run_distributed_query(const std::string& sql, const sql::logical::Catalog& schema_catalog,
                                              const sql::statistics::StatisticsCatalog& stats_catalog,
                                              const sql::storage::Database& local_full_database,
                                              const std::vector<WorkerAddress>& workers,
                                              sql::integration::CacheClient& cache,
                                              sql::integration::CacheVersions versions,
                                              sql::adaptive::BanditModel& bandit);

} // namespace sql::distributed
