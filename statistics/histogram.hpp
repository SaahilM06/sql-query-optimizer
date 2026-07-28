#pragma once

#include <algorithm>
#include <vector>

namespace sql::statistics {

// One equi-width bucket: the fraction of (non-null) rows whose value falls
// in [lower, upper].
struct HistogramBucket {
    double lower;
    double upper;
    double frequency; // e.g. 0.10 == 10% of rows
};

// Equi-width histogram (v1 -- equi-depth buckets, where each bucket holds
// roughly the same row count instead of the same value-width, are a later
// refinement noted in the spec but not implemented here).
//
// Frequencies across all buckets are expected to sum to ~1.0 (the non-null
// fraction of the column). Used only for range-comparison selectivity
// (<, <=, >, >=) -- equality selectivity uses the NDV-based formula in
// SelectivityEstimator instead, since a single equi-width bucket doesn't
// carry enough information to estimate a single value's frequency without
// an additional per-bucket distinct-count assumption the spec doesn't ask
// for.
struct Histogram {
    std::vector<HistogramBucket> buckets;

    // Fraction of rows with value in [low_bound, high_bound], by summing
    // each bucket's overlap with that range, prorated linearly across the
    // bucket's width. Clamped to [0, 1].
    double range_selectivity(double low_bound, double high_bound) const {
        double total = 0.0;
        for (const auto& b : buckets) {
            double overlap_low = std::max(b.lower, low_bound);
            double overlap_high = std::min(b.upper, high_bound);
            if (overlap_high <= overlap_low) continue;

            double width = b.upper - b.lower;
            if (width <= 0.0) {
                // Degenerate single-point bucket: counts fully if inside range.
                if (b.lower >= low_bound && b.lower <= high_bound) total += b.frequency;
                continue;
            }
            total += b.frequency * (overlap_high - overlap_low) / width;
        }
        return std::clamp(total, 0.0, 1.0);
    }
};

} // namespace sql::statistics
