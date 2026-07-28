#pragma once

#include <string>

#include "statistics_catalog.hpp"
#include "table_stats.hpp"

namespace sql::statistics {

// Parses one table's statistics from a JSON file. Throws std::runtime_error
// on malformed JSON or an unreadable file.
//
// Expected shape:
//   {
//     "row_count": 10000,
//     "page_count": 157,
//     "columns": {
//       "id": { "distinct_count": 10000, "null_fraction": 0.0, "min": 1, "max": 10000 },
//       "country": {
//         "distinct_count": 20,
//         "null_fraction": 0.01,
//         "histogram": { "buckets": [ { "lower": 0, "upper": 25, "frequency": 0.4 }, ... ] }
//       }
//     }
//   }
TableStats load_table_stats_from_file(const std::string& path);

// Loads every "<table>.json" file directly inside dir_path into a
// StatisticsCatalog, keyed by filename stem (e.g. "orders.json" ->
// table "orders"). Throws std::runtime_error if the directory can't be
// read or any file in it fails to parse.
StatisticsCatalog load_catalog_from_directory(const std::string& dir_path);

} // namespace sql::statistics
