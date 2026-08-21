#include "value_ops.hpp"

#include <stdexcept>

namespace sql::execution {

bool literal_is_null(const Literal& v) { return v.kind == Literal::Kind::Null; }

bool literal_truthy(const Literal& v) {
    if (v.kind != Literal::Kind::Boolean) throw std::runtime_error("execution: expected a boolean value");
    return v.bool_val;
}

double literal_as_double(const Literal& v) {
    if (v.kind == Literal::Kind::Integer) return static_cast<double>(v.int_val);
    if (v.kind == Literal::Kind::Float) return v.float_val;
    throw std::runtime_error("execution: expected a numeric value");
}

std::string literal_to_string(const Literal& v) {
    switch (v.kind) {
        case Literal::Kind::Integer: return std::to_string(v.int_val);
        case Literal::Kind::Float: return std::to_string(v.float_val);
        case Literal::Kind::Str: return v.str_val;
        case Literal::Kind::Boolean: return v.bool_val ? "true" : "false";
        case Literal::Kind::Null: return "<null>";
    }
    return "";
}

namespace {
bool is_numeric(const Literal& v) { return v.kind == Literal::Kind::Integer || v.kind == Literal::Kind::Float; }
bool both_int(const Literal& a, const Literal& b) {
    return a.kind == Literal::Kind::Integer && b.kind == Literal::Kind::Integer;
}
} // namespace

bool literal_equal(const Literal& a, const Literal& b) {
    if (literal_is_null(a) || literal_is_null(b)) return false;
    if (is_numeric(a) && is_numeric(b)) return literal_as_double(a) == literal_as_double(b);
    if (a.kind == Literal::Kind::Str && b.kind == Literal::Kind::Str) return a.str_val == b.str_val;
    if (a.kind == Literal::Kind::Boolean && b.kind == Literal::Kind::Boolean) return a.bool_val == b.bool_val;
    return false;
}

bool literal_less(const Literal& a, const Literal& b) {
    if (literal_is_null(a) || literal_is_null(b)) return false;
    if (is_numeric(a) && is_numeric(b)) return literal_as_double(a) < literal_as_double(b);
    if (a.kind == Literal::Kind::Str && b.kind == Literal::Kind::Str) return a.str_val < b.str_val;
    throw std::runtime_error("execution: cannot order-compare these value kinds");
}

Literal literal_add(const Literal& a, const Literal& b) {
    if (both_int(a, b)) return Literal::integer(a.int_val + b.int_val);
    return Literal::floating(literal_as_double(a) + literal_as_double(b));
}
Literal literal_sub(const Literal& a, const Literal& b) {
    if (both_int(a, b)) return Literal::integer(a.int_val - b.int_val);
    return Literal::floating(literal_as_double(a) - literal_as_double(b));
}
Literal literal_mul(const Literal& a, const Literal& b) {
    if (both_int(a, b)) return Literal::integer(a.int_val * b.int_val);
    return Literal::floating(literal_as_double(a) * literal_as_double(b));
}
Literal literal_div(const Literal& a, const Literal& b) { return Literal::floating(literal_as_double(a) / literal_as_double(b)); }

} // namespace sql::execution
