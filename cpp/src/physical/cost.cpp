#include "cost.hpp"

#include <algorithm>
#include <cmath>

namespace sql::physical::cost {

double pages_for(size_t row_count, size_t avg_row_bytes) {
    double bytes = static_cast<double>(row_count) * static_cast<double>(avg_row_bytes);
    return std::max(1.0, std::ceil(bytes / static_cast<double>(kPageSizeBytes)));
}

double seq_scan(size_t row_count, size_t avg_row_bytes) {
    return pages_for(row_count, avg_row_bytes) * kSeqPageCost +
           static_cast<double>(row_count) * kCpuTupleCost;
}

// Each matched row is treated as its own random-I/O probe (pessimistic --
// real indexes benefit from clustering/caching we don't model yet).
double index_scan(size_t matched_rows, size_t /*avg_row_bytes*/) {
    return static_cast<double>(matched_rows) * kRandomPageCost +
           static_cast<double>(matched_rows) * (kCpuTupleCost + kIndexTupleCost);
}

double filter(size_t input_rows) {
    return static_cast<double>(input_rows) * kCpuTupleCost;
}

// Local cost of testing every candidate pair. The caller additionally adds
// left.cost + left.rows * right.cost, since nested loop join re-invokes the
// inner (right) subplan once per outer (left) row -- that re-scan is what
// makes nested loop join expensive on two large inputs.
double nested_loop_join(size_t left_rows, size_t right_rows) {
    return static_cast<double>(left_rows) * static_cast<double>(right_rows) * kCpuTupleCost * 0.01;
}

// Local cost of building a hash table on the smaller side and probing it
// with the larger side. The caller adds left.cost + right.cost -- unlike
// nested loop join, each side is read exactly once.
double hash_join(size_t left_rows, size_t right_rows) {
    size_t build_rows = std::min(left_rows, right_rows);
    size_t probe_rows = std::max(left_rows, right_rows);
    return static_cast<double>(build_rows) * kHashBuildCost +
           static_cast<double>(probe_rows) * kCpuTupleCost;
}

double hash_aggregate(size_t input_rows) {
    return static_cast<double>(input_rows) * kHashBuildCost;
}

double sort(size_t input_rows) {
    double n = static_cast<double>(std::max<size_t>(input_rows, 1));
    return n * std::log2(n) * kCpuTupleCost;
}

double project(size_t input_rows) {
    return static_cast<double>(input_rows) * kCpuTupleCost * 0.1;
}

double limit(size_t input_rows) {
    return static_cast<double>(input_rows) * kCpuTupleCost * 0.01;
}

} // namespace sql::physical::cost
