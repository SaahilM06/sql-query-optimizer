#pragma once

#include <optional>
#include <string>
#include <vector>

#include "../logical/logical_plan.hpp"
#include "../parser/ast.hpp"
#include "../statistics/statistics_catalog.hpp"
#include "selectivity_estimator.hpp"

namespace sql::optimizer {

// One estimator result: not just a number, but enough context to explain
// where it came from -- which matters once a chosen plan looks wrong and
// someone (a person today, an adaptive feedback loop later) needs to know
// whether to trust it.
struct Estimate {
    double rows = 0.0;
    double confidence = 1.0; // rough 0..1 signal, not a statistical guarantee
    std::string reasoning;
};

// Identifies the single base table a logical subtree scans (walking
// through Filter wrappers). Returns nullopt for anything spanning more
// than one table -- a Join, most obviously -- since there's no single
// table's statistics to resolve a bare/qualified column against in that
// case.
struct SingleTableScope {
    std::string table_name;
    std::string alias_or_name;
};
std::optional<SingleTableScope> resolve_single_table(const sql::logical::LogicalPlan& scope);

// Estimates output row counts for logical plan nodes using real
// statistics where they can be resolved, falling back to documented
// heuristics otherwise. Deliberately separate from CostModel: cardinality
// is a property of what a node produces, not how it's computed, so the
// same estimate is reused across every physical strategy considered for a
// given node (a HashJoin and a NestedLoopJoin over the same two inputs
// produce the same row count -- only their cost differs).
class CardinalityEstimator {
public:
    explicit CardinalityEstimator(const sql::statistics::StatisticsCatalog& catalog) : catalog_(catalog) {}

    Estimate estimate_scan(const std::string& table_name) const;

    Estimate estimate_filter(double input_rows, const sql::parser::Expression& predicate,
                              const sql::logical::LogicalPlan& scope) const;

    Estimate estimate_join(double left_rows, double right_rows, const sql::parser::Expression& condition,
                            const sql::logical::LogicalPlan& left_scope,
                            const sql::logical::LogicalPlan& right_scope) const;

    Estimate estimate_aggregate(double input_rows, const std::vector<sql::parser::Expression>& group_by,
                                 const sql::logical::LogicalPlan& scope) const;

    static Estimate estimate_project(double input_rows);
    static Estimate estimate_sort(double input_rows);
    static Estimate estimate_limit(double input_rows, size_t count);

private:
    const sql::statistics::StatisticsCatalog& catalog_;
    SelectivityEstimator selectivity_;
};

} // namespace sql::optimizer
