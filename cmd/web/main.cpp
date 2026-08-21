// Entry point for the visual plan explorer: starts the HTTP server, wires
// up the JSON API, and serves the static frontend in web/frontend/. Same
// startup sequence as cmd/cli/main.cpp (schema, statistics, data,
// versions) -- this is a second front end onto the same pipeline, not a
// separate system.

#include <cstdlib>
#include <iostream>

#include "integration/cache_key.hpp"
#include "logical/schema.hpp"
#include "statistics/statistics_loader.hpp"
#include "storage/database.hpp"
#include "web/api_handlers.hpp"
#include "web/http_server.hpp"

int main() {
    auto schema_catalog = sql::logical::Catalog::with_test_tables();

    sql::statistics::StatisticsCatalog stats_catalog;
    try {
        stats_catalog = sql::statistics::load_catalog_from_directory(SQL_OPTIMIZER_STATS_DIR);
    } catch (const std::exception& e) {
        std::cout << "Warning: failed to load statistics: " << e.what() << "\n";
    }

    auto versions = sql::integration::compute_versions(schema_catalog, SQL_OPTIMIZER_STATS_DIR);
    auto database = sql::storage::load_database_from_directory(SQL_OPTIMIZER_DATA_DIR, schema_catalog);

    std::string cache_addr = "127.0.0.1:6380";
    if (const char* env = std::getenv("SQLOPT_CACHE_ADDR")) cache_addr = env;
    size_t colon = cache_addr.find(':');
    std::string cache_host = colon == std::string::npos ? cache_addr : cache_addr.substr(0, colon);
    int cache_port = colon == std::string::npos ? 6380 : std::stoi(cache_addr.substr(colon + 1));

    std::string host = "127.0.0.1";
    if (const char* env = std::getenv("SQLOPT_BIND_HOST")) host = env;
    int port = 8080;
    if (const char* env = std::getenv("SQLOPT_WEB_PORT")) port = std::stoi(env);

    sql::web::HttpServer server(host, port);
    sql::web::register_routes(server, schema_catalog, stats_catalog, database, versions, cache_host, cache_port,
                               SQL_OPTIMIZER_WEB_FRONTEND_DIR, SQL_OPTIMIZER_METRICS_LOG);

    std::cout << "Visual plan explorer: http://" << host << ":" << port << "\n";
    std::cout << "(plan cache at " << cache_addr << "; run cache/cmd/server if it's not up -- queries still work "
                 "without it, just uncached)\n";

    server.run();
    return 0;
}
