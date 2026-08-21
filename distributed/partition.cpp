#include "partition.hpp"

#include <filesystem>

#include "../parser/ast.hpp"
#include "../storage/csv_reader.hpp"

namespace sql::distributed {

size_t partition_of(int64_t id, size_t num_workers) {
    if (num_workers == 0) return 0;
    int64_t m = id % static_cast<int64_t>(num_workers);
    if (m < 0) m += static_cast<int64_t>(num_workers); // ids are always positive in this project's data, but stay correct regardless
    return static_cast<size_t>(m);
}

namespace {

sql::storage::Table filter_table_to_partition(const sql::storage::Table& full, size_t worker_id, size_t num_workers) {
    const auto& column_names = full.column_names();
    size_t id_col = column_names.size();
    for (size_t i = 0; i < column_names.size(); ++i) {
        if (column_names[i] == "id") {
            id_col = i;
            break;
        }
    }
    if (id_col == column_names.size()) return full; // no "id" column -- documented fallback: load in full

    std::vector<sql::storage::Row> owned;
    for (const auto& row : full.rows()) {
        const auto& id_literal = row[id_col];
        if (id_literal.kind != sql::parser::Literal::Kind::Integer) continue; // not partitionable, skip defensively
        if (partition_of(id_literal.int_val, num_workers) == worker_id) owned.push_back(row);
    }
    return sql::storage::Table(column_names, std::move(owned));
}

} // namespace

sql::storage::Database load_worker_partition(const std::string& data_dir, const sql::logical::Catalog& schema_catalog,
                                              size_t worker_id, size_t num_workers) {
    sql::storage::Database db;
    for (const auto& name : schema_catalog.table_names()) {
        std::filesystem::path path = std::filesystem::path(data_dir) / (name + ".csv");
        if (!std::filesystem::exists(path)) continue;
        const auto* schema = schema_catalog.get(name);
        if (schema == nullptr) continue;
        sql::storage::Table full = sql::storage::load_table_from_csv(path.string(), *schema);
        db.add_table(name, filter_table_to_partition(full, worker_id, num_workers));
    }
    return db;
}

sql::storage::Database filter_database_to_partition(const sql::storage::Database& full,
                                                      const sql::logical::Catalog& schema_catalog, size_t worker_id,
                                                      size_t num_workers) {
    sql::storage::Database db;
    for (const auto& name : schema_catalog.table_names()) {
        const auto* table = full.get(name);
        if (table == nullptr) continue;
        db.add_table(name, filter_table_to_partition(*table, worker_id, num_workers));
    }
    return db;
}

} // namespace sql::distributed
