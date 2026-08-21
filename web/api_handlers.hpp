#pragma once

#include <string>

#include "../integration/cache_key.hpp"
#include "../logical/schema.hpp"
#include "../statistics/statistics_catalog.hpp"
#include "../storage/database.hpp"
#include "http_server.hpp"

namespace sql::web {

// Registers the JSON API (GET /api/schema, POST /api/query) and a
// static-file fallback (serving `frontend_dir`) onto `server`.
//
// schema_catalog/stats_catalog/database are read-only for the server's
// lifetime -- there's no ANALYZE-equivalent endpoint yet (see
// ROADMAP.md) -- so sharing references across the server's per-connection
// threads needs no synchronization. Each request opens its own short-lived
// CacheClient rather than sharing one: CacheClient isn't thread-safe, and
// this avoids needing a lock around a connection that's reused across
// concurrent requests.
void register_routes(HttpServer& server, const sql::logical::Catalog& schema_catalog,
                      const sql::statistics::StatisticsCatalog& stats_catalog, const sql::storage::Database& database,
                      sql::integration::CacheVersions versions, const std::string& cache_host, int cache_port,
                      const std::string& frontend_dir, const std::string& metrics_log_path);

} // namespace sql::web
