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
  src/parser/lexer.cpp
  src/parser/parser.cpp
  src/logical/schema.cpp
  src/logical/planner.cpp
  src/logical/optimizer.cpp
  src/physical/cost.cpp
  src/physical/physical_planner.cpp
  src/statistics/statistics_loader.cpp
  src/optimizer/selectivity_estimator.cpp
  src/optimizer/cardinality_estimator.cpp
  src/optimizer/cost_model.cpp
  src/optimizer/explain.cpp
  src/optimizer/join_graph.cpp
  src/optimizer/access_path_generator.cpp
  src/optimizer/join_enumerator.cpp
)

target_compile_definitions(sql_optimizer_lib PUBLIC
  SQL_OPTIMIZER_STATS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/stats"
)

target_include_directories(sql_optimizer_lib PUBLIC src)

# ── Demo executable ───────────────────────────────────────────────────────────

add_executable(sql_optimizer_demo src/main.cpp)
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
