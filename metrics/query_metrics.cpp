#include "query_metrics.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "../util/json.hpp"

namespace sql::metrics {

using sql::util::JsonValue;

std::string to_json(const QueryMetrics& m) {
    JsonValue v = JsonValue::make_object();
    v.object_val["query_id"] = JsonValue::make_number(static_cast<double>(m.query_id));
    v.object_val["sql"] = JsonValue::make_string(m.sql);
    v.object_val["parse_ms"] = JsonValue::make_number(m.parse_ms);
    v.object_val["cache_lookup_ms"] = JsonValue::make_number(m.cache_lookup_ms);
    v.object_val["plan_ms"] = JsonValue::make_number(m.plan_ms);
    v.object_val["cache_store_ms"] = JsonValue::make_number(m.cache_store_ms);
    v.object_val["cache_hit"] = JsonValue::make_bool(m.cache_hit);
    v.object_val["execution_ms"] = JsonValue::make_number(m.execution_ms);
    v.object_val["executed"] = JsonValue::make_bool(m.executed);
    v.object_val["actual_rows"] = JsonValue::make_number(static_cast<double>(m.actual_rows));
    v.object_val["estimated_rows"] = JsonValue::make_number(static_cast<double>(m.estimated_rows));
    v.object_val["estimated_cost"] = JsonValue::make_number(m.estimated_cost);
    return sql::util::to_json(v);
}

void append_metrics(const std::string& path, const QueryMetrics& m) {
    std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());

    std::ofstream file(path, std::ios::app);
    if (!file) throw std::runtime_error("metrics: could not open file for append: " + path);
    file << to_json(m) << "\n";
}

} // namespace sql::metrics
