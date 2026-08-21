#pragma once

#include <optional>
#include <string>
#include <vector>

#include "../parser/ast.hpp"

namespace sql::execution {

// Maps a (table-alias-or-name, column) reference to its index within a Row
// produced by an Executor -- lets expression evaluation resolve `c.name` or
// a bare `name` against whatever columns are actually present at that point
// in the operator tree, which changes shape after a Join concatenates two
// input schemas or a Project narrows to just the selected expressions.
class RowSchema {
public:
    void add(std::optional<std::string> table, std::string column);

    // If `table` is given, matches table+column exactly. If `table` is
    // empty, matches by column name alone -- if more than one entry shares
    // that column name (e.g. both join sides have an "id" column),
    // resolution fails, matching how an unqualified ambiguous reference is
    // invalid SQL.
    std::optional<size_t> resolve(const std::optional<std::string>& table, const std::string& column) const;

    // Registers that a Function-kind expression `func(arg)` (e.g.
    // `SUM(o.total)`) already has its value materialized at row index `idx`
    // -- lets a HAVING/ORDER BY expression that re-mentions an aggregate
    // directly, instead of through its output alias, resolve against a
    // HashAggregate's output the same way a plain Column resolves via
    // resolve(). The logical planner doesn't rewrite HAVING/ORDER BY
    // expressions to reference the aggregate's alias (see
    // LogicalPlanner::plan), so this is genuinely needed, not defensive.
    void register_aggregate(std::string func, const sql::parser::Expression& arg, size_t idx);
    std::optional<size_t> resolve_aggregate(const std::string& func, const sql::parser::Expression& arg) const;

    // Appends every entry of `other` (columns and registered aggregates)
    // after this schema's own -- builds a Join's combined schema from its
    // two inputs.
    void extend(const RowSchema& other);

    size_t size() const { return entries_.size(); }
    const std::string& column_name(size_t index) const { return entries_[index].column; }

    // "table.column" if this entry has a table qualifier, else just
    // "column" -- the wire format distributed/coordinator.cpp round-trips
    // column identity through (see ExternalRows in executor_builder.cpp),
    // since a worker-injected row set needs to preserve exactly which
    // qualifier (if any) each column resolves under, not just its name.
    std::string qualified_name(size_t index) const {
        const Entry& e = entries_[index];
        return e.table.has_value() ? *e.table + "." + e.column : e.column;
    }

private:
    struct Entry {
        std::optional<std::string> table;
        std::string column;
    };
    struct AggEntry {
        std::string func; // uppercase, matching AggregateExpr::func
        sql::parser::Expression arg;
        size_t idx;
    };
    std::vector<Entry> entries_;
    std::vector<AggEntry> agg_entries_;
};

} // namespace sql::execution
