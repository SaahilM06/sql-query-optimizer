#include "row_schema.hpp"

#include <cctype>

namespace sql::execution {

using sql::parser::Expression;
using sql::parser::Literal;

namespace {

std::string to_upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool literal_equal_structural(const Literal& a, const Literal& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case Literal::Kind::Integer: return a.int_val == b.int_val;
        case Literal::Kind::Float: return a.float_val == b.float_val;
        case Literal::Kind::Str: return a.str_val == b.str_val;
        case Literal::Kind::Boolean: return a.bool_val == b.bool_val;
        case Literal::Kind::Null: return true;
    }
    return false;
}

// Structural equality between two expression trees -- used to recognize
// that a HAVING/ORDER BY expression's `SUM(o.total)` is the "same"
// aggregate as the one an ancestor HashAggregate already computed, without
// requiring the SELECT list to have aliased it.
bool expr_equal_structural(const Expression& a, const Expression& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case Expression::Kind::Column: return a.table == b.table && a.column == b.column;
        case Expression::Kind::Literal: return literal_equal_structural(a.literal, b.literal);
        case Expression::Kind::BinaryOp:
            return a.binary_op == b.binary_op && expr_equal_structural(*a.left, *b.left) &&
                   expr_equal_structural(*a.right, *b.right);
        case Expression::Kind::UnaryOp:
            return a.unary_op == b.unary_op && expr_equal_structural(*a.operand, *b.operand);
        case Expression::Kind::Function:
            if (a.func_name != b.func_name || a.args.size() != b.args.size()) return false;
            for (size_t i = 0; i < a.args.size(); ++i) {
                if (!expr_equal_structural(a.args[i], b.args[i])) return false;
            }
            return true;
        case Expression::Kind::Wildcard:
            return true;
    }
    return false;
}

} // namespace

void RowSchema::add(std::optional<std::string> table, std::string column) {
    entries_.push_back(Entry{std::move(table), std::move(column)});
}

void RowSchema::register_aggregate(std::string func, const Expression& arg, size_t idx) {
    agg_entries_.push_back(AggEntry{to_upper(std::move(func)), arg, idx});
}

std::optional<size_t> RowSchema::resolve_aggregate(const std::string& func, const Expression& arg) const {
    std::string func_upper = to_upper(func);
    for (const auto& e : agg_entries_) {
        if (e.func == func_upper && expr_equal_structural(e.arg, arg)) return e.idx;
    }
    return std::nullopt;
}

void RowSchema::extend(const RowSchema& other) {
    for (const auto& e : other.entries_) entries_.push_back(e);
    for (const auto& e : other.agg_entries_) agg_entries_.push_back(e);
}

std::optional<size_t> RowSchema::resolve(const std::optional<std::string>& table, const std::string& column) const {
    if (table.has_value()) {
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].column == column && entries_[i].table.value_or("") == *table) return i;
        }
        return std::nullopt;
    }
    std::optional<size_t> found;
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].column == column) {
            if (found.has_value()) return std::nullopt; // ambiguous
            found = i;
        }
    }
    return found;
}

} // namespace sql::execution
