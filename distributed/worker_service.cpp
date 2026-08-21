#include "worker_service.hpp"

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>

#include "../execution/executor_builder.hpp"
#include "../execution/query_runner.hpp"
#include "../integration/plan_serializer.hpp"
#include "../util/json.hpp"
#include "row_json.hpp"

namespace sql::distributed {

using sql::util::JsonValue;

namespace {

// An artificial per-request processing delay this worker imposes on
// itself, for simulating a straggler (one slow node in an otherwise
// healthy cluster) -- see cmd/benchmark/main.cpp's P99-under-a-straggler
// experiment. Read fresh per request rather than cached, so a benchmark
// can flip it on mid-run for a live before/after comparison. Distinct from
// distributed/http_client.cpp's SQLOPT_SIMULATED_LATENCY_MS, which delays
// every worker uniformly (simulating general network conditions) rather
// than singling one out.
int straggler_delay_ms() {
    const char* env = std::getenv("SQLOPT_WORKER_DELAY_MS");
    if (env == nullptr) return 0;
    try {
        return std::stoi(env);
    } catch (const std::exception&) {
        return 0;
    }
}

} // namespace

void register_worker_routes(sql::web::HttpServer& server, const sql::storage::Database& database) {
    server.route("POST", "/worker/execute", [&database](const sql::web::HttpRequest& req) {
        int delay_ms = straggler_delay_ms();
        if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

        JsonValue body = sql::util::parse_json(req.body);
        const auto* plan_field = body.find("plan");
        if (plan_field == nullptr) throw std::runtime_error("worker: missing 'plan' field");

        // Re-serialize the nested JsonValue back to text so
        // deserialize_plan (which takes a string) can parse it -- a small,
        // deliberate round-trip rather than adding a JsonValue-accepting
        // overload to plan_serializer for this one caller.
        auto plan = sql::integration::deserialize_plan(sql::util::to_json(*plan_field));

        sql::execution::ExternalRowSets external_rows;
        if (const auto* ext = body.find("external_rows"); ext != nullptr && ext->kind == JsonValue::Kind::Object) {
            for (const auto& [slot_str, slot_val] : ext->object_val) {
                size_t slot = static_cast<size_t>(std::stoul(slot_str));
                const auto* rows_field = slot_val.find("rows");
                external_rows[slot] = rows_field != nullptr ? rows_from_json(*rows_field) : std::vector<sql::storage::Row>{};
            }
        }

        auto executor = sql::execution::build_executor(plan, database, external_rows);
        auto result = sql::execution::run_to_completion(*executor);

        JsonValue columns = JsonValue::make_array();
        for (size_t i = 0; i < executor->schema().size(); ++i) {
            columns.array_val.push_back(JsonValue::make_string(executor->schema().qualified_name(i)));
        }

        JsonValue root = JsonValue::make_object();
        root.object_val["columns"] = std::move(columns);
        root.object_val["rows"] = rows_to_json(result.rows);
        return sql::web::HttpResponse::json(sql::util::to_json(root));
    });
}

} // namespace sql::distributed
