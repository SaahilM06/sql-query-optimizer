#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../parser/ast.hpp"
#include "logical_plan.hpp"
#include "schema.hpp"

namespace sql::logical {

using sql::parser::Expression;
using sql::parser::SelectItem;
using sql::parser::SelectStatement;
using sql::parser::TableRef;

// Translates a parsed SELECT statement into a logical plan tree. Throws
// std::runtime_error on failure, mirroring the Rust original's `Result`.
class LogicalPlanner {
public:
    explicit LogicalPlanner(const Catalog& catalog) : catalog_(catalog) {}

    LogicalPlan plan(SelectStatement stmt);

private:
    const Catalog& catalog_;
    // Maps every alias (or bare table name) used in the query -> canonical table name.
    // e.g. "c" -> "customers", "o" -> "orders"
    std::unordered_map<std::string, std::string> alias_map_;

    void register_alias(const TableRef& table_ref);

    // Walk the SELECT list and separate:
    // - plain expressions -> go into a Project node
    // - aggregate calls   -> go into an Aggregate node
    std::pair<std::vector<std::pair<Expression, std::optional<std::string>>>, std::vector<AggregateExpr>>
    split_select_list(const std::vector<SelectItem>& items);
};

} // namespace sql::logical
