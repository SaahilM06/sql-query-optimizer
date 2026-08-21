#include "table.hpp"

#include <stdexcept>

#include "csv_reader.hpp"

namespace sql::storage {

namespace {

sql::parser::Literal parse_field(const std::string& field, sql::logical::DataType type) {
    using sql::parser::Literal;
    if (field.empty()) return Literal::null();
    switch (type) {
        case sql::logical::DataType::Int:
            return Literal::integer(std::stoll(field));
        case sql::logical::DataType::Float:
            return Literal::floating(std::stod(field));
        case sql::logical::DataType::Text:
            return Literal::str(field);
        case sql::logical::DataType::Boolean:
            return Literal::boolean(field == "true" || field == "1" || field == "TRUE");
    }
    return Literal::null();
}

} // namespace

Table load_table_from_csv(const std::string& path, const sql::logical::TableSchema& schema) {
    auto raw = read_csv(path);
    if (raw.empty()) return Table({}, {});

    const std::vector<std::string>& header = raw[0];
    std::vector<sql::logical::DataType> column_types;
    column_types.reserve(header.size());
    for (const auto& name : header) {
        const auto* col = schema.get_column(name);
        if (col == nullptr) throw std::runtime_error("csv: column '" + name + "' in " + path + " not found in schema");
        column_types.push_back(col->data_type);
    }

    std::vector<Row> rows;
    rows.reserve(raw.size() > 0 ? raw.size() - 1 : 0);
    for (size_t r = 1; r < raw.size(); ++r) {
        const auto& raw_row = raw[r];
        if (raw_row.size() != header.size()) {
            throw std::runtime_error("csv: row " + std::to_string(r) + " in " + path + " has " +
                                      std::to_string(raw_row.size()) + " fields, expected " +
                                      std::to_string(header.size()));
        }
        Row row;
        row.reserve(raw_row.size());
        for (size_t c = 0; c < raw_row.size(); ++c) row.push_back(parse_field(raw_row[c], column_types[c]));
        rows.push_back(std::move(row));
    }

    return Table(header, std::move(rows));
}

} // namespace sql::storage
