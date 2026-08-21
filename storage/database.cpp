#include "database.hpp"

#include <filesystem>

namespace sql::storage {

Database load_database_from_directory(const std::string& dir, const sql::logical::Catalog& schema_catalog) {
    Database db;
    for (const auto& name : schema_catalog.table_names()) {
        std::filesystem::path path = std::filesystem::path(dir) / (name + ".csv");
        if (!std::filesystem::exists(path)) continue;
        const auto* schema = schema_catalog.get(name);
        if (schema == nullptr) continue;
        db.add_table(name, load_table_from_csv(path.string(), *schema));
    }
    return db;
}

} // namespace sql::storage
