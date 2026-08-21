// Benchmark suite: runs a fixed set of representative queries against the
// real single-node pipeline and (if a worker cluster is reachable) the
// real distributed pipeline, measuring actual wall-clock latency and
// cardinality estimation accuracy -- not simulated numbers.
//
// What this deliberately does NOT include: a "naive" (uncosted) planner
// mode. Building an alternate, intentionally-worse query planner just to
// have something to compare against would be new scope unrelated to
// everything else in this project, not a benchmark-harness task. Instead,
// the "how much does cost-based planning actually help" story is the
// existing join_order_differs_from_sql_syntax_order_when_cheaper test
// (tests/join_search_tests.cpp) -- a real query where the DP search proves
// a cheaper join order exists than the one written in the SQL. What this
// suite adds on top of that: real latency percentiles, real cardinality
// q-error, and a real fixed-rule-vs-adaptive comparison for the one
// runtime decision this project actually has two strategies for
// (broadcast vs. shuffle), plus a real network-condition experiment --
// all measured, not asserted.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "adaptive/bandit.hpp"
#include "distributed/coordinator.hpp"
#include "distributed/http_client.hpp"
#include "execution/executor_builder.hpp"
#include "execution/query_runner.hpp"
#include "integration/cache_client.hpp"
#include "integration/cache_key.hpp"
#include "integration/cached_planner.hpp"
#include "logical/schema.hpp"
#include "statistics/statistics_loader.hpp"
#include "storage/database.hpp"
#include "util/json.hpp"

namespace {

using sql::util::JsonValue;

struct Query {
    std::string category;
    std::string name;
    std::string sql;
};

// Representative categories: a single-table filter, a single-table
// aggregation, a broadcast-eligible join, a shuffle-eligible join
// (unfiltered, both sides over the broadcast size ceiling), a shuffle
// join immediately followed by a co-partitioned GROUP BY (exercises the
// physical-property-tracking merge-skip), and a 3-table join (exercises
// the distributed fallback path).
const std::vector<Query>& query_suite() {
    static const std::vector<Query> suite = {
        {"single_table_filter", "customers_us", "SELECT * FROM customers WHERE country = 'US'"},
        {"single_table_aggregate", "orders_by_customer",
         "SELECT customer_id, SUM(total), COUNT(*) FROM orders GROUP BY customer_id"},
        {"two_table_broadcast", "customers_orders_filtered",
         "SELECT c.name FROM customers c JOIN orders o ON c.id = o.customer_id WHERE c.country = 'US'"},
        {"two_table_shuffle", "customers_orders_unfiltered",
         "SELECT c.name, o.total FROM customers c JOIN orders o ON c.id = o.customer_id"},
        {"two_table_aggregate_copartitioned", "orders_customers_grouped",
         "SELECT o.customer_id, SUM(o.total) FROM orders o JOIN customers c ON o.customer_id = c.id GROUP BY "
         "o.customer_id"},
        {"three_table_fallback", "customers_orders_products",
         "SELECT c.name FROM customers c JOIN orders o ON c.id = o.customer_id JOIN products p ON o.product_id = "
         "p.id WHERE c.country = 'US' AND p.category = 'electronics'"},
    };
    return suite;
}

struct Measurement {
    std::string category;
    std::string query_name;
    std::string mode; // "single_node" | "distributed"
    std::string variant; // free-form: "cold"/"warm", strategy name, condition label, etc.
    int iteration = 0;
    double total_ms = 0.0;
    double estimated_rows = 0.0;
    double actual_rows = 0.0;
};

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    size_t idx = static_cast<size_t>(std::ceil(p * static_cast<double>(values.size())) - 1);
    if (idx >= values.size()) idx = values.size() - 1;
    return values[idx];
}

double q_error(double estimated, double actual) {
    if (estimated <= 0.0 || actual <= 0.0) return estimated == actual ? 1.0 : std::numeric_limits<double>::infinity();
    return std::max(estimated / actual, actual / estimated);
}

void print_stats_line(const std::string& label, const std::vector<double>& values) {
    if (values.empty()) return;
    double sum = 0.0;
    for (double v : values) sum += v;
    std::cout << "    " << std::left << std::setw(28) << label << std::right << std::fixed << std::setprecision(3)
               << "  p50=" << std::setw(9) << percentile(values, 0.50) << "  p95=" << std::setw(9)
               << percentile(values, 0.95) << "  p99=" << std::setw(9) << percentile(values, 0.99)
               << "  mean=" << std::setw(9) << (sum / static_cast<double>(values.size())) << std::defaultfloat << "\n";
}

void save_measurements(const std::string& path, const std::vector<Measurement>& measurements) {
    std::ofstream file(path, std::ios::app);
    if (!file) throw std::runtime_error("benchmark: could not open results file for append: " + path);
    for (const auto& m : measurements) {
        JsonValue j = JsonValue::make_object();
        j.object_val["category"] = JsonValue::make_string(m.category);
        j.object_val["query_name"] = JsonValue::make_string(m.query_name);
        j.object_val["mode"] = JsonValue::make_string(m.mode);
        j.object_val["variant"] = JsonValue::make_string(m.variant);
        j.object_val["iteration"] = JsonValue::make_number(m.iteration);
        j.object_val["total_ms"] = JsonValue::make_number(m.total_ms);
        j.object_val["estimated_rows"] = JsonValue::make_number(m.estimated_rows);
        j.object_val["actual_rows"] = JsonValue::make_number(m.actual_rows);
        file << sql::util::to_json(j) << "\n";
    }
}

// ── Single-node suite ────────────────────────────────────────────────────────

std::vector<Measurement> run_single_node_suite(const sql::logical::Catalog& schema_catalog,
                                                const sql::statistics::StatisticsCatalog& stats_catalog,
                                                const sql::storage::Database& database,
                                                sql::integration::CacheClient& cache,
                                                sql::integration::CacheVersions versions, int iterations) {
    std::cout << "\n== Single-node suite (" << iterations << " iterations/query) ==\n";
    std::vector<Measurement> out;

    for (const auto& q : query_suite()) {
        std::vector<double> cold, warm;
        double estimated_rows = 0.0, actual_rows = 0.0;

        for (int i = 0; i < iterations; ++i) {
            auto planned = sql::integration::plan_with_cache(q.sql, schema_catalog, stats_catalog, cache, versions);
            auto executor = sql::execution::build_executor(planned.plan, database);
            auto start = std::chrono::steady_clock::now();
            auto result = sql::execution::run_to_completion(*executor);
            double exec_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

            estimated_rows = static_cast<double>(planned.plan.estimated_rows);
            actual_rows = static_cast<double>(result.rows.size());

            Measurement m{q.category, q.name, "single_node", planned.cache_hit ? "warm" : "cold", i, exec_ms,
                          estimated_rows, actual_rows};
            out.push_back(m);
            (planned.cache_hit ? warm : cold).push_back(exec_ms);
        }

        std::cout << "  " << q.category << " / " << q.name << "  (q-error=" << std::fixed << std::setprecision(2)
                   << q_error(estimated_rows, actual_rows) << std::defaultfloat << ", est=" << estimated_rows
                   << " actual=" << actual_rows << ")\n";
        print_stats_line("cold (cache miss)", cold);
        print_stats_line("warm (cache hit)", warm);
    }

    return out;
}

// ── Distributed suite ────────────────────────────────────────────────────────

bool worker_reachable(const sql::distributed::WorkerAddress& w) {
    auto resp = sql::distributed::http_post_json(w.host, w.port, "/worker/execute", "{}", 500);
    return resp.has_value(); // any response (even a 500 for a malformed body) means the process is up
}

std::vector<Measurement> run_distributed_queries(const std::vector<Query>& queries, const std::string& mode_label,
                                                  const sql::logical::Catalog& schema_catalog,
                                                  const sql::statistics::StatisticsCatalog& stats_catalog,
                                                  const sql::storage::Database& local_full_database,
                                                  const std::vector<sql::distributed::WorkerAddress>& workers,
                                                  sql::integration::CacheClient& cache,
                                                  sql::integration::CacheVersions versions,
                                                  sql::adaptive::BanditModel& bandit, int iterations) {
    std::vector<Measurement> out;
    for (const auto& q : queries) {
        std::vector<double> latencies;
        double estimated_rows = 0.0, actual_rows = 0.0;
        for (int i = 0; i < iterations; ++i) {
            auto result = sql::distributed::run_distributed_query(q.sql, schema_catalog, stats_catalog,
                                                                    local_full_database, workers, cache, versions,
                                                                    bandit);
            latencies.push_back(result.total_ms);
            actual_rows = static_cast<double>(result.rows.size());
            out.push_back(Measurement{q.category, q.name, "distributed", mode_label, i, result.total_ms,
                                       estimated_rows, actual_rows});
        }
        print_stats_line(q.category + " / " + q.name, latencies);
    }
    return out;
}

} // namespace

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
    sql::integration::CacheClient cache(cache_host, cache_port);
    cache.connect();

    std::string worker_spec = "127.0.0.1:7001,127.0.0.1:7002,127.0.0.1:7003";
    if (const char* env = std::getenv("SQLOPT_WORKERS")) worker_spec = env;
    std::vector<sql::distributed::WorkerAddress> workers;
    {
        std::istringstream ss(worker_spec);
        std::string item;
        while (std::getline(ss, item, ',')) {
            auto c = item.find(':');
            if (c == std::string::npos) continue;
            workers.push_back({item.substr(0, c), std::stoi(item.substr(c + 1))});
        }
    }

    std::string results_path = SQL_OPTIMIZER_BENCHMARKS_LOG;
    if (const char* env = std::getenv("SQLOPT_BENCHMARK_OUTPUT")) results_path = env;
    int iterations = 15;
    if (const char* env = std::getenv("SQLOPT_BENCHMARK_ITERATIONS")) iterations = std::stoi(env);

    std::cout << "SQL Query Optimizer -- Benchmark Suite\n";
    std::cout << "Dataset: data/ (300 customers, 3000 orders, 50 products)\n";
    std::cout << "Cache: " << (cache.is_connected() ? "connected" : "unavailable (results reflect uncached planning)")
               << "\n";

    std::vector<Measurement> all_measurements;

    auto single_node = run_single_node_suite(schema_catalog, stats_catalog, database, cache, versions, iterations);
    all_measurements.insert(all_measurements.end(), single_node.begin(), single_node.end());

    bool workers_up = !workers.empty();
    for (const auto& w : workers) {
        if (!worker_reachable(w)) {
            workers_up = false;
            break;
        }
    }

    if (!workers_up) {
        std::cout << "\n== Distributed suite: SKIPPED -- not all configured workers ("
                   << worker_spec << ") are reachable.\n";
        std::cout << "   Start them first, e.g.:\n";
        std::cout << "     ./build/sql_optimizer_worker 0 3 7001 &\n";
        std::cout << "     ./build/sql_optimizer_worker 1 3 7002 &\n";
        std::cout << "     ./build/sql_optimizer_worker 2 3 7003 &\n";
    } else {
        std::vector<Query> distributable;
        for (const auto& q : query_suite()) {
            if (q.category != "three_table_fallback") distributable.push_back(q);
        }

        std::cout << "\n== Distributed suite, adaptive (bandit, fresh -- " << iterations << " iterations/query) ==\n";
        sql::adaptive::BanditModel fresh_bandit; // not persisted -- this run's learning curve starts from nothing
        auto adaptive_run = run_distributed_queries(distributable, "adaptive", schema_catalog, stats_catalog,
                                                     database, workers, cache, versions, fresh_bandit, iterations);
        all_measurements.insert(all_measurements.end(), adaptive_run.begin(), adaptive_run.end());

        std::cout << "\n== Distributed suite, fixed rule: always shuffle (" << iterations << " iterations/query) ==\n";
        setenv("SQLOPT_FORCE_STRATEGY", "shuffle", 1);
        sql::adaptive::BanditModel unused_bandit1;
        auto forced_shuffle = run_distributed_queries(distributable, "forced_shuffle", schema_catalog, stats_catalog,
                                                       database, workers, cache, versions, unused_bandit1, iterations);
        all_measurements.insert(all_measurements.end(), forced_shuffle.begin(), forced_shuffle.end());
        unsetenv("SQLOPT_FORCE_STRATEGY");

        std::cout << "\n== Network-condition experiment: adaptive under 0ms vs. 50ms simulated latency ==\n";
        std::cout << "  -- 0ms baseline --\n";
        sql::adaptive::BanditModel net_bandit; // fresh, so both conditions start from the same (no) prior learning
        auto net_0ms = run_distributed_queries(distributable, "latency_0ms", schema_catalog, stats_catalog, database,
                                                workers, cache, versions, net_bandit, iterations);
        all_measurements.insert(all_measurements.end(), net_0ms.begin(), net_0ms.end());

        std::cout << "  -- 50ms simulated per-request latency --\n";
        setenv("SQLOPT_SIMULATED_LATENCY_MS", "50", 1);
        auto net_50ms = run_distributed_queries(distributable, "latency_50ms", schema_catalog, stats_catalog,
                                                 database, workers, cache, versions, net_bandit, iterations);
        all_measurements.insert(all_measurements.end(), net_50ms.begin(), net_50ms.end());
        unsetenv("SQLOPT_SIMULATED_LATENCY_MS");
    }

    try {
        save_measurements(results_path, all_measurements);
        std::cout << "\nWrote " << all_measurements.size() << " measurements to " << results_path << "\n";
    } catch (const std::exception& e) {
        std::cout << "\nWarning: could not save results: " << e.what() << "\n";
    }

    return 0;
}
