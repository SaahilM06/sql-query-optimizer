// Interactive REPL for the optimizer: plans a query (using the plan cache
// when reachable), then actually runs it against the small CSV-backed
// dataset in data/ and prints the result rows. EXPLAIN prints the full
// annotated physical plan without executing; EXPLAIN ANALYZE executes it
// and prints estimated vs. actual rows/time per operator. SHOW WORKERS is
// stubbed -- it needs a worker cluster, which doesn't exist yet (see
// ROADMAP.md).

#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

#include "execution/executor_builder.hpp"
#include "execution/query_runner.hpp"
#include "execution/value_ops.hpp"
#include "integration/cache_client.hpp"
#include "integration/cache_key.hpp"
#include "integration/cached_planner.hpp"
#include "logical/schema.hpp"
#include "optimizer/explain.hpp"
#include "statistics/statistics_loader.hpp"
#include "storage/database.hpp"

namespace {

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string to_upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool starts_with_ci(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    return to_upper(s.substr(0, prefix.size())) == prefix;
}

void print_help() {
    std::cout << "Commands:\n"
                 "  <sql statement>       plan (using the cache), execute against data/, print the result rows\n"
                 "  EXPLAIN <sql>         print the full annotated physical plan, without executing\n"
                 "  EXPLAIN ANALYZE <sql> execute and print estimated vs. actual rows/time per operator\n"
                 "  SHOW STATS            print loaded schema + statistics\n"
                 "  SHOW CACHE            print plan cache INFO\n"
                 "  SHOW WORKERS          not applicable -- single-node only\n"
                 "  HELP                  this message\n"
                 "  EXIT / QUIT           leave the REPL\n";
}

void show_stats(const sql::logical::Catalog& schema_catalog, const sql::statistics::StatisticsCatalog& stats_catalog) {
    for (const auto& name : schema_catalog.table_names()) {
        const auto* schema = schema_catalog.get(name);
        std::cout << name << "\n";

        std::cout << "  columns: ";
        for (size_t i = 0; i < schema->columns.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << schema->columns[i].name;
        }
        std::cout << "\n";

        if (!schema->indexed_columns.empty()) {
            std::cout << "  indexed: ";
            for (size_t i = 0; i < schema->indexed_columns.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << schema->indexed_columns[i];
            }
            std::cout << "\n";
        }

        const auto* stats = stats_catalog.get(name);
        if (stats != nullptr) {
            std::cout << "  row_count: " << static_cast<long long>(stats->row_count) << "\n";
        } else {
            std::cout << "  row_count: (no statistics loaded for this table)\n";
        }
    }
}

void show_cache(sql::integration::CacheClient& cache) {
    if (!cache.is_connected()) {
        std::cout << "Cache not connected.\n";
        return;
    }
    auto reply = cache.command({"INFO"});
    if (!reply.has_value() || reply->type != sql::integration::Reply::Type::Bulk) {
        std::cout << "Cache did not respond to INFO as expected.\n";
        return;
    }
    std::cout << reply->bulk_val << "\n";
}

void print_result_table(const sql::execution::RowSchema& schema, const std::vector<sql::storage::Row>& rows) {
    for (size_t i = 0; i < schema.size(); ++i) {
        if (i > 0) std::cout << " | ";
        std::cout << schema.column_name(i);
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

void run_bare_query(const std::string& sql, const sql::logical::Catalog& schema_catalog,
                     const sql::statistics::StatisticsCatalog& stats_catalog, sql::integration::CacheClient& cache,
                     sql::integration::CacheVersions versions, const sql::storage::Database& database) {
    auto result = sql::integration::plan_with_cache(sql, schema_catalog, stats_catalog, cache, versions);
    std::cout << "Plan cache: " << (result.cache_hit ? "HIT" : "MISS") << "\n";

    auto executor = sql::execution::build_executor(result.plan, database);
    const auto& schema = executor->schema(); // capture before run_to_completion moves nothing but keeps executor alive
    auto exec_result = sql::execution::run_to_completion(*executor);

    print_result_table(schema, exec_result.rows);
    std::cout << "(" << exec_result.rows.size() << " rows, " << std::fixed << std::setprecision(3)
               << exec_result.total_elapsed_ms << " ms, plan estimated " << result.plan.estimated_rows << " rows, cost "
               << result.plan.estimated_cost.total() << ")" << std::defaultfloat << "\n";
}

void run_explain(const std::string& sql, const sql::logical::Catalog& schema_catalog,
                  const sql::statistics::StatisticsCatalog& stats_catalog, sql::integration::CacheClient& cache,
                  sql::integration::CacheVersions versions) {
    auto result = sql::integration::plan_with_cache(sql, schema_catalog, stats_catalog, cache, versions);
    std::cout << "Plan cache: " << (result.cache_hit ? "HIT" : "MISS") << "\n\n";
    sql::optimizer::explain_plan(result.plan, std::cout);
}

void run_explain_analyze(const std::string& sql, const sql::logical::Catalog& schema_catalog,
                          const sql::statistics::StatisticsCatalog& stats_catalog, sql::integration::CacheClient& cache,
                          sql::integration::CacheVersions versions, const sql::storage::Database& database) {
    auto result = sql::integration::plan_with_cache(sql, schema_catalog, stats_catalog, cache, versions);
    std::cout << "Plan cache: " << (result.cache_hit ? "HIT" : "MISS") << "\n\n";

    auto executor = sql::execution::build_executor(result.plan, database);
    sql::execution::explain_analyze(result.plan, *executor, std::cout);
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
    bool connected = cache.connect();

    std::cout << "SQL Query Optimizer -- interactive CLI\n";
    std::cout << "Cache: " << (connected ? ("connected at " + cache_addr) : ("unavailable at " + cache_addr + " (running standalone)"))
               << "\n";
    std::cout << "Type HELP for commands.\n\n";

    std::string line;
    while (true) {
        std::cout << "sql> " << std::flush;
        if (!std::getline(std::cin, line)) break;

        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;
        while (!trimmed.empty() && trimmed.back() == ';') trimmed.pop_back();
        trimmed = trim(trimmed);
        if (trimmed.empty()) continue;

        std::string upper = to_upper(trimmed);

        if (upper == "EXIT" || upper == "QUIT") break;
        if (upper == "HELP") {
            print_help();
            continue;
        }
        if (upper == "SHOW STATS") {
            show_stats(schema_catalog, stats_catalog);
            continue;
        }
        if (upper == "SHOW CACHE") {
            show_cache(cache);
            continue;
        }
        if (upper == "SHOW WORKERS") {
            std::cout << "Not applicable -- this build is single-node only (no distributed execution yet).\n";
            continue;
        }
        if (starts_with_ci(trimmed, "EXPLAIN ANALYZE")) {
            std::string sql = trim(trimmed.substr(15));
            if (sql.empty()) {
                std::cout << "Usage: EXPLAIN ANALYZE <sql statement>\n";
                continue;
            }
            try {
                run_explain_analyze(sql, schema_catalog, stats_catalog, cache, versions, database);
            } catch (const std::exception& e) {
                std::cout << "Error: " << e.what() << "\n";
            }
            continue;
        }
        if (starts_with_ci(trimmed, "EXPLAIN")) {
            std::string sql = trim(trimmed.substr(7));
            if (sql.empty()) {
                std::cout << "Usage: EXPLAIN <sql statement>\n";
                continue;
            }
            try {
                run_explain(sql, schema_catalog, stats_catalog, cache, versions);
            } catch (const std::exception& e) {
                std::cout << "Error: " << e.what() << "\n";
            }
            continue;
        }

        try {
            run_bare_query(trimmed, schema_catalog, stats_catalog, cache, versions, database);
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << "\n";
        }
    }

    std::cout << "\nGoodbye.\n";
    return 0;
}
