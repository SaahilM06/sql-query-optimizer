#pragma once

#include <cstddef>

namespace sql::physical::cost {

// ── Cost constants ────────────────────────────────────────────────────────────
//
// Abstract cost units, not real seconds/dollars -- only relative magnitude
// matters, since every candidate for a given node is compared on the same
// scale. Modeled loosely on Postgres's cost constants (kSeqPageCost,
// kRandomPageCost etc.) but drastically simplified: no disk cache modeling,
// no parallelism, no statistics-driven row estimates yet (see
// physical_planner.cpp's TODOs -- that lands with cardinality estimation).

constexpr double kCpuTupleCost = 0.01;    // cost to process one row in memory
constexpr double kSeqPageCost = 1.0;      // cost to read one page sequentially
constexpr double kRandomPageCost = 4.0;   // cost to read one page via random I/O (index probe)
constexpr double kIndexTupleCost = 0.005; // extra per-row CPU cost of an index lookup
constexpr double kHashBuildCost = 0.02;   // per-row cost to insert into a hash table
constexpr size_t kPageSizeBytes = 8192;   // bytes per page, for page-count estimates

double pages_for(size_t row_count, size_t avg_row_bytes);

// ── Per-operator local cost (excludes children -- callers add those in) ──────

double seq_scan(size_t row_count, size_t avg_row_bytes);
double index_scan(size_t matched_rows, size_t avg_row_bytes);
double filter(size_t input_rows);
double nested_loop_join(size_t left_rows, size_t right_rows);
double hash_join(size_t left_rows, size_t right_rows);
double hash_aggregate(size_t input_rows);
double sort(size_t input_rows);
double project(size_t input_rows);
double limit(size_t input_rows);

} // namespace sql::physical::cost
