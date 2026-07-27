#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sql::logical {

// ── Column ───────────────────────────────────────────────────────────────────

enum class DataType { Int, Float, Text, Boolean };

struct ColumnDef {
    std::string name;
    DataType data_type;
    bool nullable;
};

// ── Table ────────────────────────────────────────────────────────────────────
//
// Row count + average row size in bytes.
// The costing layer uses these to estimate join output sizes and scan costs.
struct TableStats {
    size_t row_count;
    size_t avg_row_bytes;
};

struct TableSchema {
    std::vector<ColumnDef> columns;
    TableStats stats;

    const ColumnDef* get_column(const std::string& name) const {
        for (const auto& c : columns) {
            if (c.name == name) return &c;
        }
        return nullptr;
    }
};

// ── Catalog ──────────────────────────────────────────────────────────────────

class Catalog {
public:
    Catalog() = default;

    void register_table(const std::string& name, TableSchema schema) {
        tables_[name] = std::move(schema);
    }

    const TableSchema* get(const std::string& name) const {
        auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : &it->second;
    }

    /// Pre-populated catalog for development and testing.
    static Catalog with_test_tables();

private:
    std::unordered_map<std::string, TableSchema> tables_;
};

} // namespace sql::logical
