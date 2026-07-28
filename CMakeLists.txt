cmake_minimum_required(VERSION 3.16)
project(sql_query_optimizer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

# ── Core library ─────────────────────────────────────────────────────────────

add_library(sql_optimizer_lib STATIC
  parser/lexer.cpp
  parser/parser.cpp
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
)

target_compile_definitions(sql_optimizer_lib PUBLIC
  SQL_OPTIMIZER_STATS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/stats"
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

# ── Tests ─────────────────────────────────────────────────────────────────────

enable_testing()

add_executable(sql_optimizer_tests
  tests/main.cpp
  tests/parser_tests.cpp
  tests/optimizer_tests.cpp
  tests/physical_tests.cpp
  tests/statistics_tests.cpp
  tests/join_search_tests.cpp
)
target_link_libraries(sql_optimizer_tests PRIVATE sql_optimizer_lib)
target_include_directories(sql_optimizer_tests PRIVATE tests)

add_test(NAME sql_optimizer_tests COMMAND sql_optimizer_tests)
