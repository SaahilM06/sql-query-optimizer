cmake_minimum_required(VERSION 3.16)
project(sql_query_optimizer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)

# ── Core library ─────────────────────────────────────────────────────────────

add_library(sql_optimizer_lib STATIC
  util/json.cpp
  parser/lexer.cpp
  parser/parser.cpp
  parser/sql_printer.cpp
  logical/schema.cpp
  logical/planner.cpp
  logical/optimizer.cpp
  physical/cost.cpp
  physical/physical_planner.cpp
  statistics/statistics_loader.cpp
  optimizer/selectivity_estimator.cpp
  optimizer/cardinality_estimator.cpp
  optimizer/cost_model.cpp
  optimizer/explain.cpp
  optimizer/join_graph.cpp
  optimizer/access_path_generator.cpp
  optimizer/join_enumerator.cpp
  integration/plan_serializer.cpp
  integration/cache_key.cpp
  integration/cache_client.cpp
  integration/cached_planner.cpp
  storage/csv_reader.cpp
  storage/table.cpp
  storage/database.cpp
  execution/row_schema.cpp
  execution/value_ops.cpp
  execution/expr_eval.cpp
  execution/operators.cpp
  execution/executor_builder.cpp
  execution/query_runner.cpp
  statistics/analyzer.cpp
  metrics/query_metrics.cpp
  web/http_server.cpp
  web/plan_json.cpp
  web/api_handlers.cpp
  distributed/partition.cpp
  distributed/row_json.cpp
  distributed/http_client.cpp
  distributed/worker_service.cpp
  distributed/coordinator.cpp
  adaptive/calibration.cpp
  adaptive/bandit.cpp
)

target_link_libraries(sql_optimizer_lib PUBLIC Threads::Threads)

target_compile_definitions(sql_optimizer_lib PUBLIC
  SQL_OPTIMIZER_STATS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/stats"
  SQL_OPTIMIZER_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
  SQL_OPTIMIZER_METRICS_LOG="${CMAKE_CURRENT_SOURCE_DIR}/metrics/query_log.jsonl"
  SQL_OPTIMIZER_WEB_FRONTEND_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web/frontend"
  SQL_OPTIMIZER_CALIBRATION_LOG="${CMAKE_CURRENT_SOURCE_DIR}/adaptive/calibration.json"
  SQL_OPTIMIZER_BANDIT_LOG="${CMAKE_CURRENT_SOURCE_DIR}/adaptive/bandit.json"
)

# Repo root, so every component's own internal includes (e.g.
# physical/physical_plan.hpp's `#include "../logical/logical_plan.hpp"`)
# keep resolving via ordinary directory-relative lookup, while consumers
# like cmd/demo/main.cpp can include root-relative ("logical/optimizer.hpp")
# without needing a chain of "../../". PUBLIC here means both
# sql_optimizer_demo and sql_optimizer_tests inherit it automatically
# through their existing links below.
target_include_directories(sql_optimizer_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ── Demo executable ───────────────────────────────────────────────────────────

add_executable(sql_optimizer_demo cmd/demo/main.cpp)
target_link_libraries(sql_optimizer_demo PRIVATE sql_optimizer_lib)

# ── Interactive CLI ───────────────────────────────────────────────────────────

add_executable(sql_optimizer_cli cmd/cli/main.cpp)
target_link_libraries(sql_optimizer_cli PRIVATE sql_optimizer_lib)

# ── Visual plan explorer (web) ──────────────────────────────────────────────

add_executable(sql_optimizer_web cmd/web/main.cpp)
target_link_libraries(sql_optimizer_web PRIVATE sql_optimizer_lib)

# ── Distributed worker + coordinator ────────────────────────────────────────

add_executable(sql_optimizer_worker cmd/worker/main.cpp)
target_link_libraries(sql_optimizer_worker PRIVATE sql_optimizer_lib)

add_executable(sql_optimizer_coordinator cmd/coordinator/main.cpp)
target_link_libraries(sql_optimizer_coordinator PRIVATE sql_optimizer_lib)

# ── Tests ─────────────────────────────────────────────────────────────────────

enable_testing()

add_executable(sql_optimizer_tests
  tests/main.cpp
  tests/parser_tests.cpp
  tests/optimizer_tests.cpp
  tests/physical_tests.cpp
  tests/statistics_tests.cpp
  tests/join_search_tests.cpp
  tests/integration_tests.cpp
)
target_link_libraries(sql_optimizer_tests PRIVATE sql_optimizer_lib)
target_include_directories(sql_optimizer_tests PRIVATE tests)

add_test(NAME sql_optimizer_tests COMMAND sql_optimizer_tests)
