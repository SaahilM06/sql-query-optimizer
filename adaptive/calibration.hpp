#pragma once

#include <string>
#include <unordered_map>

#include "../execution/executor.hpp"
#include "../physical/physical_plan.hpp"

namespace sql::adaptive {

// Online linear regression correcting the abstract cost-model units
// (physical/cost.hpp's kCpuTupleCost etc.) into a predicted wall-clock
// time, per PhysicalPlan::Kind. The static cost model's units were never
// meant to *be* milliseconds, only to rank candidates against each other
// during planning -- this is a separate, additive layer that learns the
// actual predicted-cost -> ms relationship from real EXPLAIN ANALYZE runs.
// It never feeds back into the join-order/algorithm search itself (that
// would mean re-running the DP search with corrected coefficients, a
// larger change left for later); it only makes the number reported
// alongside a plan a better latency predictor.
class CalibrationModel {
public:
    struct Fit {
        // Online least squares via running sums -- scale/bias are
        // (re)computed from these on demand, so there's no incremental-
        // update numerical drift to worry about.
        size_t n = 0;
        double sum_x = 0.0, sum_y = 0.0, sum_xx = 0.0, sum_xy = 0.0;

        double scale() const;
        double bias() const;
    };

    // predicted_cost: a node's PhysicalPlan::estimated_cost.total().
    // actual_ms: that same node's Executor::elapsed_ms() from the same run
    // (both cumulative -- self + descendants -- so they're on the same
    // basis; see execution/executor.hpp).
    void record_observation(const std::string& kind, double predicted_cost, double actual_ms);

    // scale*predicted_cost + bias using the fitted line for `kind`, once
    // enough samples exist (kMinSamples) -- otherwise predicted_cost
    // unchanged, since a couple of noisy samples shouldn't be trusted to
    // extrapolate a slope from.
    double calibrated_estimate_ms(const std::string& kind, double predicted_cost) const;

    const std::unordered_map<std::string, Fit>& snapshot() const { return fits_; }
    void load_fits(std::unordered_map<std::string, Fit> fits) { fits_ = std::move(fits); }

private:
    std::unordered_map<std::string, Fit> fits_;
};

// Walks `plan` and `executor` (the same isomorphic pair build_executor
// produces everywhere else in this project) and records one observation
// per node into `model`.
void record_plan_observations(const sql::physical::PhysicalPlan& plan, sql::execution::Executor& executor,
                               CalibrationModel& model);

CalibrationModel load_calibration(const std::string& path);
void save_calibration(const std::string& path, const CalibrationModel& model);

} // namespace sql::adaptive
