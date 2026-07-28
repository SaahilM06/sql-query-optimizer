#include "cost.hpp"

#include <algorithm>
#include <cmath>

namespace sql::physical::cost {

double pages_for(size_t row_count, size_t avg_row_bytes) {
    double bytes = static_cast<double>(row_count) * static_cast<double>(avg_row_bytes);
    return std::max(1.0, std::ceil(bytes / static_cast<double>(kPageSizeBytes)));
}

double spill_penalty(size_t rows) {
    double r = static_cast<double>(rows);
    if (r <= kMemoryBudgetRows) return 0.0;
    return (r - kMemoryBudgetRows) * kSpillIoCostPerRow;
}

Cost seq_scan(size_t row_count, double page_count) {
    Cost c;
    c.io = std::max(1.0, page_count) * kSeqPageCost;
    c.cpu = static_cast<double>(row_count) * kCpuTupleCost;
    return c;
}

// Each matched row is treated as its own random-I/O probe (pessimistic --
// real indexes benefit from clustering/caching we don't model yet).
Cost index_scan(size_t matched_rows) {
    Cost c;
    c.io = static_cast<double>(matched_rows) * kRandomPageCost;
    c.cpu = static_cast<double>(matched_rows) * (kCpuTupleCost + kIndexTupleCost);
    return c;
}

Cost filter(size_t input_rows) {
    Cost c;
    c.cpu = static_cast<double>(input_rows) * kCpuTupleCost;
    return c;
}

// Local cost of testing every candidate pair. The caller additionally adds
// left.cost + left.rows * right.cost, since nested loop join re-invokes the
// inner (right) subplan once per outer (left) row -- that re-scan is what
// makes nested loop join expensive on two large inputs.
Cost nested_loop_join(size_t left_rows, size_t right_rows) {
    Cost c;
    c.cpu = static_cast<double>(left_rows) * static_cast<double>(right_rows) * kCpuTupleCost * 0.01;
    return c;
}

// Local cost of building a hash table on the smaller side and probing it
// with the larger side. The caller adds left.cost + right.cost -- unlike
// nested loop join, each side is read exactly once. Memory cost is
// proportional to the build side; once it exceeds kMemoryBudgetRows, a
// spill penalty is added to I/O, the same way a real hash join spills
// partitions to disk when the build side doesn't fit in memory.
Cost hash_join(size_t left_rows, size_t right_rows) {
    size_t build_rows = std::min(left_rows, right_rows);
    size_t probe_rows = std::max(left_rows, right_rows);

    Cost c;
    c.cpu = static_cast<double>(build_rows) * kHashBuildCost + static_cast<double>(probe_rows) * kCpuTupleCost;
    c.memory = static_cast<double>(build_rows) * kMemoryPerRowCost;
    c.io = spill_penalty(build_rows);
    return c;
}

// CPU is proportional to input rows scanned (every row is hashed into a
// group); memory is proportional to the number of groups actually held in
// the hash table, which is usually far smaller than the input.
Cost hash_aggregate(size_t input_rows, size_t estimated_groups) {
    Cost c;
    c.cpu = static_cast<double>(input_rows) * kHashBuildCost;
    c.memory = static_cast<double>(estimated_groups) * kMemoryPerRowCost;
    c.io = spill_penalty(estimated_groups);
    return c;
}

Cost sort(size_t input_rows) {
    double n = static_cast<double>(std::max<size_t>(input_rows, 1));
    Cost c;
    c.cpu = n * std::log2(n) * kCpuTupleCost;
    c.memory = n * kMemoryPerRowCost;
    c.io = spill_penalty(input_rows);
    return c;
}

Cost project(size_t input_rows) {
    Cost c;
    c.cpu = static_cast<double>(input_rows) * kCpuTupleCost * 0.1;
    return c;
}

Cost limit(size_t input_rows) {
    Cost c;
    c.cpu = static_cast<double>(input_rows) * kCpuTupleCost * 0.01;
    return c;
}

} // namespace sql::physical::cost
