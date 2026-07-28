#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace sql::parser {

enum class TokenKind {
    // Keywords
    Select, From, Where, Join, Inner, Left, Right, Cross, On,
    And, Or, Not, As, Order, By, Group, Limit, Asc, Desc, Having,
    Null, True, False,

    // Values (payload carried in Token::value)
    Identifier, Integer, Float, StringLit,

    // Operators
    Eq, Neq, Lt, Gt, Lte, Gte, Plus, Minus, Star, Slash,

    // Punctuation
    Comma, Dot, LParen, RParen,

    Eof,
};

// Mirrors Rust's `Token` enum: most variants carry no payload; Identifier /
// Integer / Float / StringLit carry one value in `value`.
struct Token {
    TokenKind kind;
    std::variant<std::monostate, std::string, int64_t, double> value;

    Token() : kind(TokenKind::Eof), value(std::monostate{}) {}
    explicit Token(TokenKind k) : kind(k), value(std::monostate{}) {}
    Token(TokenKind k, std::string v) : kind(k), value(std::move(v)) {}
    Token(TokenKind k, int64_t v) : kind(k), value(v) {}
    Token(TokenKind k, double v) : kind(k), value(v) {}

    const std::string& text() const { return std::get<std::string>(value); }
    int64_t integer() const { return std::get<int64_t>(value); }
    double floating() const { return std::get<double>(value); }

    bool operator==(const Token& other) const {
        return kind == other.kind && value == other.value;
    }
    bool operator!=(const Token& other) const { return !(*this == other); }
};

inline Token make_identifier(std::string s) { return Token(TokenKind::Identifier, std::move(s)); }
inline Token make_integer(int64_t v) { return Token(TokenKind::Integer, v); }
inline Token make_float(double v) { return Token(TokenKind::Float, v); }
inline Token make_string_lit(std::string s) { return Token(TokenKind::StringLit, std::move(s)); }

} // namespace sql::parser
