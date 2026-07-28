#pragma once

#include <string>
#include <unordered_map>

#include "column_stats.hpp"

namespace sql::statistics {

struct TableStats {
    double row_count = 0.0;
    double page_count = 0.0;
    std::unordered_map<std::string, ColumnStats> columns;

    const ColumnStats* get_column(const std::string& name) const {
        auto it = columns.find(name);
        return it == columns.end() ? nullptr : &it->second;
    }
};

} // namespace sql::statistics
