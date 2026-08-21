// The distributed coordinator: an interactive REPL, same shape as
// cmd/cli/main.cpp, but running queries via distributed::run_distributed_query
// across a set of worker processes instead of locally. Usage:
//   sql_optimizer_coordinator [host:port,host:port,...]
// or set SQLOPT_WORKERS the same way.

#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "adaptive/bandit.hpp"
#include "distributed/coordinator.hpp"
#include "execution/value_ops.hpp"
#include "integration/cache_client.hpp"
#include "integration/cache_key.hpp"
#include "logical/schema.hpp"
#include "statistics/statistics_loader.hpp"
#include "storage/database.hpp"

namespace {

std::vector<sql::distributed::WorkerAddress> parse_workers(const std::string& spec) {
    std::vector<sql::distributed::WorkerAddress> out;
    std::istringstream ss(spec);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto colon = item.find(':');
        if (colon == std::string::npos) continue;
        out.push_back({item.substr(0, colon), std::stoi(item.substr(colon + 1))});
    }
    return out;
}

void print_result_table(const std::vector<std::string>& columns, const std::vector<sql::storage::Row>& rows) {
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) std::cout << " | ";
        std::cout << columns[i];
    }
    std::cout << "\n";
    for (const auto& row : rows) {
        for (size_t i = 0; i < row.size(); ++i) {
            if (i > 0) std::cout << " | ";
            std::cout << sql::execution::literal_to_string(row[i]);
        }
        std::cout << "\n";
    }
}

void show_bandit(const sql::adaptive::BanditModel& bandit) {
    const auto& snapshot = bandit.snapshot();
    if (snapshot.empty()) {
        std::cout << "No bandit data yet -- run a broadcast/shuffle-eligible join a few times first.\n";
        return;
    }
    for (const auto& [context, arms] : snapshot) {
        std::cout << context << ":\n";
        for (const auto& [arm, stats] : arms) {
            std::cout << "  " << arm << ": " << stats.count << " observation(s), avg reward "
                       << std::fixed << std::setprecision(3) << stats.average_reward << std::defaultfloat
                       << " (i.e. ~" << -stats.average_reward << " ms)\n";
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string worker_spec = "127.0.0.1:7001,127.0.0.1:7002,127.0.0.1:7003";
    if (const char* env = std::getenv("SQLOPT_WORKERS")) worker_spec = env;
    if (argc > 1) worker_spec = argv[1];
    auto workers = parse_workers(worker_spec);

    auto schema_catalog = sql::logical::Catalog::with_test_tables();
    sql::statistics::StatisticsCatalog stats_catalog;
    try {
        stats_catalog = sql::statistics::load_catalog_from_directory(SQL_OPTIMIZER_STATS_DIR);
    } catch (const std::exception& e) {
        std::cout << "Warning: failed to load statistics: " << e.what() << "\n";
    }
    auto versions = sql::integration::compute_versions(schema_catalog, SQL_OPTIMIZER_STATS_DIR);
    // Loaded in full, unlike a worker's static partition -- used only as
    // the single-node fallback path when a query isn't eligible for
    // distribution (see distributed/coordinator.cpp).
    auto local_full_database = sql::storage::load_database_from_directory(SQL_OPTIMIZER_DATA_DIR, schema_catalog);

    std::string cache_addr = "127.0.0.1:6380";
    if (const char* env = std::getenv("SQLOPT_CACHE_ADDR")) cache_addr = env;
    size_t colon = cache_addr.find(':');
    std::string cache_host = colon == std::string::npos ? cache_addr : cache_addr.substr(0, colon);
    int cache_port = colon == std::string::npos ? 6380 : std::stoi(cache_addr.substr(colon + 1));
    sql::integration::CacheClient cache(cache_host, cache_port);
    bool cache_connected = cache.connect();

    auto bandit = sql::adaptive::load_bandit(SQL_OPTIMIZER_BANDIT_LOG);

    std::cout << "Distributed coordinator -- " << workers.size() << " worker(s):\n";
    for (const auto& w : workers) std::cout << "  " << w.host << ":" << w.port << "\n";
    std::cout << "Cache: " << (cache_connected ? ("connected at " + cache_addr) : "unavailable") << "\n";
    std::cout << "Type SQL, SHOW BANDIT, or EXIT/QUIT.\n\n";

    std::string line;
    while (true) {
        std::cout << "sql> " << std::flush;
        if (!std::getline(std::cin, line)) break;

        std::string trimmed = line;
        while (!trimmed.empty() && (trimmed.back() == ';' || trimmed.back() == ' ')) trimmed.pop_back();
        if (trimmed.empty()) continue;

        std::string upper = trimmed;
        for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (upper == "EXIT" || upper == "QUIT") break;
        if (upper == "SHOW BANDIT") {
            show_bandit(bandit);
            continue;
        }

        try {
            auto result = sql::distributed::run_distributed_query(trimmed, schema_catalog, stats_catalog,
                                                                    local_full_database, workers, cache, versions,
                                                                    bandit);
            if (result.distributed) {
                std::cout << "[distributed";
                if (!result.join_strategy.empty()) std::cout << ", strategy=" << result.join_strategy;
                if (result.used_copartition_merge_skip) std::cout << ", co-partition merge skipped";
                std::cout << ", " << result.workers_used << " worker(s), " << result.total_ms << " ms]\n";
            } else {
                std::cout << "[fallback to single-node -- " << result.fallback_reason << " (" << result.total_ms
                          << " ms)]\n";
            }
            print_result_table(result.columns, result.rows);
            std::cout << "(" << result.rows.size() << " rows)\n";

            try {
                sql::adaptive::save_bandit(SQL_OPTIMIZER_BANDIT_LOG, bandit);
            } catch (const std::exception& e) {
                std::cout << "(warning: could not save bandit state: " << e.what() << ")\n";
            }
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << "\n";
        }
    }

    std::cout << "\nGoodbye.\n";
    return 0;
}
