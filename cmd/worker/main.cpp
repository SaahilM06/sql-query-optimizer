// A distributed-execution worker: owns a static partition of every table
// (hash(id) % num_workers, decided at startup -- see distributed/
// partition.cpp) and exposes it over HTTP to a coordinator. Usage:
//   sql_optimizer_worker <worker_id> <num_workers> [port]

#include <cstdlib>
#include <iostream>
#include <string>

#include "distributed/partition.hpp"
#include "distributed/worker_service.hpp"
#include "logical/schema.hpp"
#include "web/http_server.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cout << "Usage: " << argv[0] << " <worker_id> <num_workers> [port]\n";
        return 1;
    }
    size_t worker_id = static_cast<size_t>(std::stoul(argv[1]));
    size_t num_workers = static_cast<size_t>(std::stoul(argv[2]));
    int port = argc > 3 ? std::stoi(argv[3]) : static_cast<int>(7001 + worker_id);

    if (worker_id >= num_workers) {
        std::cout << "worker_id must be less than num_workers\n";
        return 1;
    }

    auto schema_catalog = sql::logical::Catalog::with_test_tables();
    auto database = sql::distributed::load_worker_partition(SQL_OPTIMIZER_DATA_DIR, schema_catalog, worker_id, num_workers);

    std::cout << "Worker " << worker_id << "/" << num_workers << " -- owns:\n";
    for (const auto& name : schema_catalog.table_names()) {
        const auto* table = database.get(name);
        std::cout << "  " << name << ": " << (table != nullptr ? table->row_count() : 0) << " rows\n";
    }

    std::string host = "127.0.0.1";
    if (const char* env = std::getenv("SQLOPT_BIND_HOST")) host = env;

    sql::web::HttpServer server(host, port);
    sql::distributed::register_worker_routes(server, database);

    std::cout << "Listening on http://" << host << ":" << port << "\n";
    server.run();
    return 0;
}
