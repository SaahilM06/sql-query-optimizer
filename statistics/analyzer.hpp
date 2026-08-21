#pragma once

#include "../logical/schema.hpp"
#include "../storage/database.hpp"
#include "statistics_catalog.hpp"

namespace sql::statistics {

// Computes real TableStats/ColumnStats (row/page counts, NDV, null
// fraction, min/max, an equi-width histogram) by scanning `table`'s actual
// rows -- the real-data counterpart to load_table_stats_from_file's
// hand-authored JSON fixtures, matching the "ANALYZE <table>" idea from
// ROADMAP.md. `schema` supplies each column's DataType, so a numeric
// column gets min/max/histogram and a text/boolean column doesn't (a
// histogram over strings isn't meaningful for this project's
// range-selectivity use, per Histogram's own doc comment).
TableStats analyze_table(const sql::storage::Table& table, const sql::logical::TableSchema& schema);

// Runs analyze_table for every table present in both `db` and
// `schema_catalog`.
StatisticsCatalog analyze_database(const sql::storage::Database& db, const sql::logical::Catalog& schema_catalog);

} // namespace sql::statistics
