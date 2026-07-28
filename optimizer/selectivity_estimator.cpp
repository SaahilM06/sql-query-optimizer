#include "selectivity_estimator.hpp"

#include <algorithm>
#include <limits>
#include <optional>

namespace sql::optimizer {

using namespace sql::parser;
using namespace sql::statistics;

namespace {

std::optional<double> literal_as_number(const Literal& lit) {
    switch (lit.kind) {
        case Literal::Kind::Integer: return static_cast<double>(lit.int_val);
        case Literal::Kind::Float: return lit.float_val;
        default: return std::nullopt; // strings/booleans/null aren't range-comparable here
    }
}

} // namespace

double SelectivityEstimator::estimate(const Expression& predicate, const TableStats& stats) const {
    switch (predicate.kind) {
        case Expression::Kind::BinaryOp:
            switch (predicate.binary_op) {
                case BinaryOperator::And:
                    return std::clamp(estimate(*predicate.left, stats) * estimate(*predicate.right, stats), 0.0, 1.0);
                case BinaryOperator::Or: {
                    double a = estimate(*predicate.left, stats);
                    double b = estimate(*predicate.right, stats);
                    return std::clamp(a + b - a * b, 0.0, 1.0);
                }
                case BinaryOperator::Eq:
                case BinaryOperator::Neq:
                case BinaryOperator::Lt:
                case BinaryOperator::Lte:
                case BinaryOperator::Gt:
                case BinaryOperator::Gte:
                    return estimate_comparison(predicate, stats);
                default:
                    // Add/Sub/Mul/Div reaching here isn't a boolean predicate.
                    return kDefaultSelectivity;
            }
        case Expression::Kind::UnaryOp:
            if (predicate.unary_op == UnaryOperator::Not) {
                return std::clamp(1.0 - estimate(*predicate.operand, stats), 0.0, 1.0);
            }
            return kDefaultSelectivity; // Neg isn't boolean
        default:
            return kDefaultSelectivity;
    }
}

double SelectivityEstimator::estimate_comparison(const Expression& predicate, const TableStats& stats) const {
    const Expression* col_side = nullptr;
    const Expression* lit_side = nullptr;
    bool literal_on_right = true;

    if (predicate.left->kind == Expression::Kind::Column && predicate.right->kind == Expression::Kind::Literal) {
        col_side = predicate.left.get();
        lit_side = predicate.right.get();
        literal_on_right = true;
    } else if (predicate.right->kind == Expression::Kind::Column && predicate.left->kind == Expression::Kind::Literal) {
        col_side = predicate.right.get();
        lit_side = predicate.left.get();
        literal_on_right = false;
    } else {
        return kDefaultSelectivity; // column-to-column, function call, etc.
    }

    BinaryOperator op = predicate.binary_op;

    if (op == BinaryOperator::Eq) {
        return estimate_equality(col_side->column, stats);
    }
    if (op == BinaryOperator::Neq) {
        return std::clamp(1.0 - estimate_equality(col_side->column, stats), 0.0, 1.0);
    }

    auto lit_num = literal_as_number(lit_side->literal);
    if (!lit_num.has_value()) return kDefaultSelectivity;

    // Normalize to "column <op> value": `100 < total` means the same thing
    // as `total > 100`, so flip the operator when the literal was written
    // on the left.
    BinaryOperator normalized_op = op;
    if (!literal_on_right) {
        switch (op) {
            case BinaryOperator::Lt: normalized_op = BinaryOperator::Gt; break;
            case BinaryOperator::Lte: normalized_op = BinaryOperator::Gte; break;
            case BinaryOperator::Gt: normalized_op = BinaryOperator::Lt; break;
            case BinaryOperator::Gte: normalized_op = BinaryOperator::Lte; break;
            default: break;
        }
    }

    return estimate_range(col_side->column, normalized_op, *lit_num, stats);
}

double SelectivityEstimator::estimate_equality(const std::string& column, const TableStats& stats) const {
    const ColumnStats* cs = stats.get_column(column);
    if (cs == nullptr || cs->distinct_count <= 0.0) return kDefaultSelectivity;
    double usable = std::max(0.0, 1.0 - cs->null_fraction);
    return std::clamp(usable / cs->distinct_count, 0.0, 1.0);
}

double SelectivityEstimator::estimate_range(const std::string& column, BinaryOperator op, double value,
                                             const TableStats& stats) const {
    const ColumnStats* cs = stats.get_column(column);
    if (cs == nullptr) return kDefaultSelectivity;

    // Prefer the histogram when one exists: bucket frequencies already
    // reflect the real distribution instead of assuming uniformity.
    if (cs->histogram.has_value()) {
        double low = -std::numeric_limits<double>::infinity();
        double high = std::numeric_limits<double>::infinity();
        switch (op) {
            case BinaryOperator::Gt:
            case BinaryOperator::Gte:
                low = value;
                break;
            case BinaryOperator::Lt:
            case BinaryOperator::Lte:
                high = value;
                break;
            default:
                return kDefaultSelectivity;
        }
        // Histogram bucket frequencies are defined to already sum to the
        // non-null fraction of rows, so no further null adjustment here.
        return cs->histogram->range_selectivity(low, high);
    }

    if (!cs->min_value.has_value() || !cs->max_value.has_value()) return kDefaultSelectivity;
    double min_v = *cs->min_value;
    double max_v = *cs->max_value;
    if (max_v <= min_v) return kDefaultSelectivity;

    double usable = std::max(0.0, 1.0 - cs->null_fraction);
    double frac;
    switch (op) {
        case BinaryOperator::Gt:
        case BinaryOperator::Gte:
            if (value >= max_v) frac = 0.0;
            else if (value <= min_v) frac = 1.0;
            else frac = (max_v - value) / (max_v - min_v);
            break;
        case BinaryOperator::Lt:
        case BinaryOperator::Lte:
            if (value <= min_v) frac = 0.0;
            else if (value >= max_v) frac = 1.0;
            else frac = (value - min_v) / (max_v - min_v);
            break;
        default:
            return kDefaultSelectivity;
    }
    return std::clamp(frac * usable, 0.0, 1.0);
}

} // namespace sql::optimizer
