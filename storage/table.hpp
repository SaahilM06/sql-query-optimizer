#pragma once

#include <string>
#include <vector>

#include "../logical/schema.hpp"
#include "row.hpp"

namespace sql::storage {

// An in-memory, fully-materialized table -- the real-execution counterpart
// to logical::TableSchema/statistics::TableStats, which describe a table's
// shape and numeric summary respectively but hold no actual data.
class Table {
public:
    Table() = default;
    Table(std::vector<std::string> column_names, std::vector<Row> rows)
        : column_names_(std::move(column_names)), rows_(std::move(rows)) {}

    const std::vector<std::string>& column_names() const { return column_names_; }
    const std::vector<Row>& rows() const { return rows_; }
    size_t row_count() const { return rows_.size(); }

private:
    std::vector<std::string> column_names_;
    std::vector<Row> rows_;
};

// Loads a table's data from a CSV file. The header row's column names are
// matched against `schema` to determine each field's DataType for parsing;
// an empty field becomes a NULL literal. Throws std::runtime_error if the
// file can't be read, a header column isn't in `schema`, or a data row has
// the wrong number of fields.
Table load_table_from_csv(const std::string& path, const sql::logical::TableSchema& schema);

} // namespace sql::storage
