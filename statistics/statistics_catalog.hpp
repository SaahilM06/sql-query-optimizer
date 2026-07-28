#pragma once

#include <string>
#include <unordered_map>

#include "table_stats.hpp"

namespace sql::statistics {

// Central store of per-table statistics used by SelectivityEstimator and
// CardinalityEstimator. Distinct from sql::logical::Catalog, which holds
// schema/type/index metadata needed for parsing and binding -- this
// catalog holds the numeric statistics needed for cost estimation
// (row counts, distinct values, histograms), typically loaded from JSON
// files rather than hand-written alongside the schema.
class StatisticsCatalog {
public:
    StatisticsCatalog() = default;

    void register_table(const std::string& name, TableStats stats) {
        tables_[name] = std::move(stats);
    }

    const TableStats* get(const std::string& name) const {
        auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<std::string, TableStats> tables_;
};

} // namespace sql::statistics
