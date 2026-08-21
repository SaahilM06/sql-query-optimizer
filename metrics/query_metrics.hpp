#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sql::metrics {

// Per-query telemetry: how long each planning phase took, whether the plan
// cache was hit, and (when the query was actually executed, not just
// planned/explained) how long that took and how the estimate compared to
// reality. The per-query-ID, per-phase-timing shape mirrors the "Query
// #4281 / Planning / Cache / Execution" breakdown in ROADMAP.md's
// observability section.
struct QueryMetrics {
    uint64_t query_id = 0;
    std::string sql;

    double parse_ms = 0.0;
    double cache_lookup_ms = 0.0;
    double plan_ms = 0.0;        // 0.0 on a cache hit -- nothing was (re)planned
    double cache_store_ms = 0.0; // 0.0 on a cache hit -- nothing was (re)stored
    bool cache_hit = false;

    double execution_ms = 0.0; // 0.0 if the query was only planned/explained, not executed
    bool executed = false;
    size_t actual_rows = 0;

    size_t estimated_rows = 0;
    double estimated_cost = 0.0;
};

std::string to_json(const QueryMetrics& m);

// Appends one JSON object per line to `path` (created if it doesn't exist
// yet) -- the same one-line-per-record shape as cache/benchmarks/
// results.jsonl, so both sides of this project keep an appendable,
// diffable performance history rather than a single mutable snapshot.
// Throws std::runtime_error if the file can't be opened for append.
void append_metrics(const std::string& path, const QueryMetrics& m);

} // namespace sql::metrics
