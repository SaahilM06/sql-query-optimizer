#include "api_handlers.hpp"

#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "../execution/executor_builder.hpp"
#include "../execution/query_runner.hpp"
#include "../integration/cache_client.hpp"
#include "../integration/cached_planner.hpp"
#include "../metrics/query_metrics.hpp"
#include "../util/json.hpp"
#include "plan_json.hpp"

namespace sql::web {

using sql::util::JsonValue;

namespace {

std::atomic<uint64_t> g_next_query_id{1};
std::mutex g_metrics_mutex; // guards concurrent appends to the shared metrics log file

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("web: could not open file: " + path);
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::string content_type_for(const std::string& path) {
    auto ends_with = [&](const char* suffix) {
        size_t len = std::string(suffix).size();
        return path.size() >= len && path.compare(path.size() - len, len, suffix) == 0;
    };
    if (ends_with(".html")) return "text/html";
    if (ends_with(".js")) return "application/javascript";
    if (ends_with(".css")) return "text/css";
    return "application/octet-stream";
}

void log_metrics_best_effort(const std::string& path, const sql::metrics::QueryMetrics& m) {
    std::lock_guard<std::mutex> lock(g_metrics_mutex);
    try {
        sql::metrics::append_metrics(path, m);
    } catch (const std::exception&) {
        // Best-effort: a metrics-logging failure shouldn't fail the request.
    }
}

} // namespace

void register_routes(HttpServer& server, const sql::logical::Catalog& schema_catalog,
                      const sql::statistics::StatisticsCatalog& stats_catalog, const sql::storage::Database& database,
                      sql::integration::CacheVersions versions, const std::string& cache_host, int cache_port,
                      const std::string& frontend_dir, const std::string& metrics_log_path) {
    server.route("GET", "/api/schema", [&schema_catalog, &stats_catalog](const HttpRequest&) {
        JsonValue tables = JsonValue::make_array();
        for (const auto& name : schema_catalog.table_names()) {
            const auto* schema = schema_catalog.get(name);
            JsonValue t = JsonValue::make_object();
            t.object_val["name"] = JsonValue::make_string(name);

            JsonValue cols = JsonValue::make_array();
            for (const auto& col : schema->columns) cols.array_val.push_back(JsonValue::make_string(col.name));
            t.object_val["columns"] = std::move(cols);

            const auto* stats = stats_catalog.get(name);
            t.object_val["row_count"] = JsonValue::make_number(stats != nullptr ? stats->row_count : 0.0);
            tables.array_val.push_back(std::move(t));
        }
        JsonValue root = JsonValue::make_object();
        root.object_val["tables"] = std::move(tables);
        return HttpResponse::json(sql::util::to_json(root));
    });

    server.route("POST", "/api/query", [&schema_catalog, &stats_catalog, &database, versions, cache_host, cache_port,
                                         metrics_log_path](const HttpRequest& req) {
        JsonValue body = sql::util::parse_json(req.body);
        const auto* sql_field = body.find("sql");
        if (sql_field == nullptr || sql_field->kind != JsonValue::Kind::String) {
            JsonValue err = JsonValue::make_object();
            err.object_val["error"] = JsonValue::make_string("missing 'sql' field");
            return HttpResponse::json(sql::util::to_json(err), 400);
        }
        std::string sql = sql_field->str_val;
        bool execute = false;
        if (const auto* ex = body.find("execute")) execute = ex->as_bool(false);

        sql::integration::CacheClient cache(cache_host, cache_port);
        bool connected = cache.connect();

        auto result = sql::integration::plan_with_cache(sql, schema_catalog, stats_catalog, cache, versions);

        std::unique_ptr<sql::execution::Executor> executor;
        sql::execution::ExecutionResult exec_result;
        if (execute) {
            executor = sql::execution::build_executor(result.plan, database);
            exec_result = sql::execution::run_to_completion(*executor);
        }

        JsonValue timing = JsonValue::make_object();
        timing.object_val["parse_ms"] = JsonValue::make_number(result.parse_ms);
        timing.object_val["cache_lookup_ms"] = JsonValue::make_number(result.cache_lookup_ms);
        timing.object_val["plan_ms"] = JsonValue::make_number(result.plan_ms);
        timing.object_val["cache_store_ms"] = JsonValue::make_number(result.cache_store_ms);
        if (execute) timing.object_val["execution_ms"] = JsonValue::make_number(exec_result.total_elapsed_ms);

        JsonValue root = JsonValue::make_object();
        root.object_val["cache_hit"] = JsonValue::make_bool(result.cache_hit);
        root.object_val["cache_connected"] = JsonValue::make_bool(connected);
        root.object_val["timing"] = std::move(timing);
        if (execute) root.object_val["row_count"] = JsonValue::make_number(static_cast<double>(exec_result.rows.size()));
        root.object_val["plan"] = plan_to_json_tree(result.plan, executor.get());

        uint64_t query_id = g_next_query_id++;
        root.object_val["query_id"] = JsonValue::make_number(static_cast<double>(query_id));

        sql::metrics::QueryMetrics m;
        m.query_id = query_id;
        m.sql = sql;
        m.parse_ms = result.parse_ms;
        m.cache_lookup_ms = result.cache_lookup_ms;
        m.plan_ms = result.plan_ms;
        m.cache_store_ms = result.cache_store_ms;
        m.cache_hit = result.cache_hit;
        m.estimated_rows = result.plan.estimated_rows;
        m.estimated_cost = result.plan.estimated_cost.total();
        if (execute) {
            m.executed = true;
            m.execution_ms = exec_result.total_elapsed_ms;
            m.actual_rows = exec_result.rows.size();
        }
        log_metrics_best_effort(metrics_log_path, m);

        return HttpResponse::json(sql::util::to_json(root));
    });

    server.set_fallback([frontend_dir](const HttpRequest& req) {
        std::string rel = (req.path == "/") ? "/index.html" : req.path;
        // Not a general static file server -- this only ever serves the
        // fixed, small set of files this project ships under
        // web/frontend/, but rejecting ".." is a cheap guard against the
        // obvious path-traversal case anyway.
        if (rel.find("..") != std::string::npos) return HttpResponse::text("bad path", 400);
        std::string path = frontend_dir + rel;
        try {
            return HttpResponse::text(read_file(path), 200, content_type_for(path));
        } catch (const std::exception&) {
            return HttpResponse::text("not found", 404);
        }
    });
}

} // namespace sql::web
