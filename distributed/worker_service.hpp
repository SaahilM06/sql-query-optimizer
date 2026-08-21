#pragma once

#include "../storage/database.hpp"
#include "../web/http_server.hpp"

namespace sql::distributed {

// Registers POST /worker/execute onto `server`: given a serialized
// PhysicalPlan (optionally with external_rows to inject at ExternalRows
// leaves), builds an executor against `database` (this worker's local
// partition) and returns the resulting rows as JSON. This is the only
// route a worker exposes -- gathering a broadcast side, computing a
// worker's post-shuffle bucket, and running the final local join/aggregate
// are all just different plans sent to the same endpoint.
void register_worker_routes(sql::web::HttpServer& server, const sql::storage::Database& database);

} // namespace sql::distributed
