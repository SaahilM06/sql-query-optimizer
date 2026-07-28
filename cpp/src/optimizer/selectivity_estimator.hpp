#pragma once

#include "../parser/ast.hpp"
#include "../statistics/table_stats.hpp"

namespace sql::optimizer {

// Default selectivity used when a predicate's shape or referenced column
// can't be resolved against statistics (a cross-column comparison, a
// function call, a column with no stats registered, ...). A deliberately
// unconfident middle-of-the-road guess rather than 0 or 1 -- similar in
// spirit to PostgreSQL's DEFAULT_EQ_SEL / DEFAULT_INEQ_SEL constants.
constexpr double kDefaultSelectivity = 0.3;

// Estimates the fraction of a table's rows that survive a WHERE/HAVING
// predicate, using real column statistics (distinct counts, null
// fractions, min/max, histograms) where available.
//
// Only predicate shapes the parser can actually produce are handled:
// `column <op> literal` (or the reverse) combined with AND/OR/NOT.
// BETWEEN and IS [NOT] NULL aren't part of the current SQL grammar, so
// there's nothing to estimate for them yet.
class SelectivityEstimator {
public:
    double estimate(const sql::parser::Expression& predicate, const sql::statistics::TableStats& stats) const;

private:
    double estimate_comparison(const sql::parser::Expression& predicate, const sql::statistics::TableStats& stats) const;
    double estimate_equality(const std::string& column, const sql::statistics::TableStats& stats) const;
    double estimate_range(const std::string& column, sql::parser::BinaryOperator op, double value,
                           const sql::statistics::TableStats& stats) const;
};

} // namespace sql::optimizer
