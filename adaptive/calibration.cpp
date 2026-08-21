#include "calibration.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "../util/json.hpp"

namespace sql::adaptive {

using sql::util::JsonValue;

namespace {

constexpr size_t kMinSamples = 3;

// Independent of optimizer/explain.cpp's, integration/plan_serializer.cpp's,
// and web/plan_json.cpp's own private copies of the same mapping --
// deliberate small duplication (each already exists for a different
// output format) rather than exposing one of them or adding a new shared
// utility for a fourth caller.
const char* kind_name(sql::physical::PhysicalPlan::Kind k) {
    using Kind = sql::physical::PhysicalPlan::Kind;
    switch (k) {
        case Kind::SeqScan: return "SeqScan";
        case Kind::IndexScan: return "IndexScan";
        case Kind::Filter: return "Filter";
        case Kind::NestedLoopJoin: return "NestedLoopJoin";
        case Kind::HashJoin: return "HashJoin";
        case Kind::IndexNestedLoopJoin: return "IndexNestedLoopJoin";
        case Kind::HashAggregate: return "HashAggregate";
        case Kind::Project: return "Project";
        case Kind::Sort: return "Sort";
        case Kind::Limit: return "Limit";
        case Kind::ExternalRows: return "ExternalRows";
    }
    return "?";
}

void walk(const sql::physical::PhysicalPlan& plan, sql::execution::Executor& executor, CalibrationModel& model) {
    model.record_observation(kind_name(plan.kind), plan.estimated_cost.total(), executor.elapsed_ms());

    std::vector<const sql::physical::PhysicalPlan*> plan_children;
    if (plan.input) plan_children.push_back(plan.input.get());
    if (plan.left) plan_children.push_back(plan.left.get());
    if (plan.right) plan_children.push_back(plan.right.get());
    auto exec_children = executor.children();
    for (size_t i = 0; i < plan_children.size() && i < exec_children.size(); ++i) {
        walk(*plan_children[i], *exec_children[i], model);
    }
}

} // namespace

double CalibrationModel::Fit::scale() const {
    if (n < kMinSamples) return 1.0;
    double denom = static_cast<double>(n) * sum_xx - sum_x * sum_x;
    // No variance in predicted_cost across samples (e.g. the same query
    // analyzed repeatedly, so every observation has the same x) -- there's
    // no slope to fit, only a mean to report. Falling back to scale=1
    // here (as if there were a slope) would feed a bogus bias below.
    if (denom == 0.0) return 0.0;
    return (static_cast<double>(n) * sum_xy - sum_x * sum_y) / denom;
}

double CalibrationModel::Fit::bias() const {
    if (n < kMinSamples) return 0.0;
    double denom = static_cast<double>(n) * sum_xx - sum_x * sum_x;
    if (denom == 0.0) return sum_y / static_cast<double>(n); // no slope -- predict the observed mean actual_ms
    return (sum_y - scale() * sum_x) / static_cast<double>(n);
}

void CalibrationModel::record_observation(const std::string& kind, double predicted_cost, double actual_ms) {
    Fit& f = fits_[kind];
    f.n += 1;
    f.sum_x += predicted_cost;
    f.sum_y += actual_ms;
    f.sum_xx += predicted_cost * predicted_cost;
    f.sum_xy += predicted_cost * actual_ms;
}

double CalibrationModel::calibrated_estimate_ms(const std::string& kind, double predicted_cost) const {
    auto it = fits_.find(kind);
    if (it == fits_.end() || it->second.n < kMinSamples) return predicted_cost;
    return it->second.scale() * predicted_cost + it->second.bias();
}

void record_plan_observations(const sql::physical::PhysicalPlan& plan, sql::execution::Executor& executor,
                               CalibrationModel& model) {
    walk(plan, executor, model);
}

CalibrationModel load_calibration(const std::string& path) {
    CalibrationModel model;
    std::ifstream file(path);
    if (!file) return model;

    std::stringstream ss;
    ss << file.rdbuf();
    JsonValue root;
    try {
        root = sql::util::parse_json(ss.str());
    } catch (const std::exception&) {
        return model;
    }

    std::unordered_map<std::string, CalibrationModel::Fit> fits;
    if (root.kind == JsonValue::Kind::Object) {
        for (const auto& [kind, fit_json] : root.object_val) {
            CalibrationModel::Fit f;
            if (const auto* v = fit_json.find("n")) f.n = static_cast<size_t>(v->as_number());
            if (const auto* v = fit_json.find("sum_x")) f.sum_x = v->as_number();
            if (const auto* v = fit_json.find("sum_y")) f.sum_y = v->as_number();
            if (const auto* v = fit_json.find("sum_xx")) f.sum_xx = v->as_number();
            if (const auto* v = fit_json.find("sum_xy")) f.sum_xy = v->as_number();
            fits[kind] = f;
        }
    }
    model.load_fits(std::move(fits));
    return model;
}

void save_calibration(const std::string& path, const CalibrationModel& model) {
    JsonValue root = JsonValue::make_object();
    for (const auto& [kind, f] : model.snapshot()) {
        JsonValue fit_json = JsonValue::make_object();
        fit_json.object_val["n"] = JsonValue::make_number(static_cast<double>(f.n));
        fit_json.object_val["sum_x"] = JsonValue::make_number(f.sum_x);
        fit_json.object_val["sum_y"] = JsonValue::make_number(f.sum_y);
        fit_json.object_val["sum_xx"] = JsonValue::make_number(f.sum_xx);
        fit_json.object_val["sum_xy"] = JsonValue::make_number(f.sum_xy);
        root.object_val[kind] = std::move(fit_json);
    }
    std::ofstream file(path);
    if (!file) throw std::runtime_error("adaptive: could not open calibration state file for write: " + path);
    file << sql::util::to_json(root);
}

} // namespace sql::adaptive
