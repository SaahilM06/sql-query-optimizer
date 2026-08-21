#pragma once

#include <string>
#include <unordered_map>

#include "../logical/schema.hpp"
#include "table.hpp"

namespace sql::storage {

// An in-memory collection of loaded tables, keyed by name -- what the
// execution engine actually scans, as opposed to logical::Catalog (schema)
// or statistics::StatisticsCatalog (numeric summaries), neither of which
// holds real rows.
class Database {
public:
    void add_table(std::string name, Table table) { tables_[std::move(name)] = std::move(table); }

    const Table* get(const std::string& name) const {
        auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<std::string, Table> tables_;
};

// Loads "<dir>/<table>.csv" for every table registered in schema_catalog
// that has a matching file. A table with no CSV file simply isn't present
// in the returned Database -- a query touching it fails at execution time
// with a clear error rather than silently returning zero rows.
Database load_database_from_directory(const std::string& dir, const sql::logical::Catalog& schema_catalog);

} // namespace sql::storage
