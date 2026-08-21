#pragma once

#include <string>

#include "../parser/ast.hpp"

namespace sql::execution {

using sql::parser::Literal;

bool literal_is_null(const Literal& v);
bool literal_truthy(const Literal& v);       // requires Boolean kind, throws otherwise
double literal_as_double(const Literal& v);  // Integer/Float only, throws otherwise
std::string literal_to_string(const Literal& v);  // for display + as a grouping/hash key

// NULL never compares equal or less than anything, including another NULL
// -- three-valued SQL logic simplified to "NULL predicates are false"
// rather than modeling UNKNOWN separately, a deliberate v1 simplification.
bool literal_equal(const Literal& a, const Literal& b);
bool literal_less(const Literal& a, const Literal& b);

// Int+Int stays Int; anything involving a Float promotes to Float. Division
// always produces Float, avoiding integer-division surprises.
Literal literal_add(const Literal& a, const Literal& b);
Literal literal_sub(const Literal& a, const Literal& b);
Literal literal_mul(const Literal& a, const Literal& b);
Literal literal_div(const Literal& a, const Literal& b);

} // namespace sql::execution
