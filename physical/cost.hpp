#pragma once

#include <cstddef>

namespace sql::physical::cost {

// ── Cost constants ────────────────────────────────────────────────────────────
//
// Abstract cost units, not real seconds/dollars -- only relative magnitude
// matters, since every candidate for a given node is compared on the same
// scale. Modeled loosely on Postgres's cost constants (kSeqPageCost,
// kRandomPageCost etc.) but drastically simplified: no disk cache modeling,
// no parallelism.

constexpr double kCpuTupleCost = 0.01;    // cost to process one row in memory
constexpr double kSeqPageCost = 1.0;      // cost to read one page sequentially
constexpr double kRandomPageCost = 4.0;   // cost to read one page via random I/O (index probe)
constexpr double kIndexTupleCost = 0.005; // extra per-row CPU cost of an index lookup
constexpr double kHashBuildCost = 0.02;   // per-row cost to insert into a hash table
constexpr size_t kPageSizeBytes = 8192;   // bytes per page, for page-count fallback estimates

constexpr double kMemoryPerRowCost = 0.001;  // abstract per-row memory-pressure unit
constexpr double kMemoryBudgetRows = 100000; // rows a hash table/sort buffer holds before "spilling"
constexpr double kSpillIoCostPerRow = 0.05;  // extra I/O cost per row once spilled to disk

// Cost broken into its CPU, I/O, and memory-pressure components. Kept
// separate (rather than one scalar) so a memory-heavy plan and an
// I/O-heavy plan are visibly different in an explain/print output, even
// when their totals happen to be close.
struct Cost {
    double cpu = 0.0;
    double io = 0.0;
    double memory = 0.0;

    double total() const { return cpu + io + memory; }

    Cost operator+(const Cost& other) const { return Cost{cpu + other.cpu, io + other.io, memory + other.memory}; }
    Cost& operator+=(const Cost& other) {
        cpu += other.cpu;
        io += other.io;
        memory += other.memory;
        return *this;
    }
};

/// Fallback page-count estimate when a table's real page_count isn't known
/// (e.g. no statistics registered for it).
double pages_for(size_t row_count, size_t avg_row_bytes);

/// Extra I/O modeling the cost of spilling to disk once an in-memory
/// structure (a hash table, a sort buffer) grows past kMemoryBudgetRows.
double spill_penalty(size_t rows);

// ── Per-operator local cost (excludes children -- callers add those in) ──────

Cost seq_scan(size_t row_count, double page_count);
Cost index_scan(size_t matched_rows);
Cost filter(size_t input_rows);
Cost nested_loop_join(size_t left_rows, size_t right_rows);
Cost hash_join(size_t left_rows, size_t right_rows);
Cost index_nested_loop_join(size_t outer_rows, size_t output_rows);
Cost hash_aggregate(size_t input_rows, size_t estimated_groups);
Cost sort(size_t input_rows);
Cost project(size_t input_rows);
Cost limit(size_t input_rows);

} // namespace sql::physical::cost
