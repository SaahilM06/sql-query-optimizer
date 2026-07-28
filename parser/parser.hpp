#pragma once

#include <vector>

#include "ast.hpp"
#include "token.hpp"

namespace sql::parser {

// Recursive-descent parser. Errors surface as std::runtime_error, mirroring
// the Rust original's `Result<_, String>` error path.
//
// Expression precedence (lowest → highest), enforced by call hierarchy:
//   parse_or → parse_and → parse_not → parse_comparison → parse_additive
//   → parse_multiplicative → parse_unary → parse_primary
class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    Statement parse();
    Expression parse_expr();

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;

    const Token& peek() const;
    Token advance();
    void expect(TokenKind expected);

    Statement parse_select();
    std::vector<SelectItem> parse_select_list();
    TableRef parse_table_ref();
    Join parse_join();
    std::vector<OrderByItem> parse_order_by_list();
    std::vector<Expression> parse_expr_list();

    Expression parse_or();
    Expression parse_and();
    Expression parse_not();
    Expression parse_comparison();
    Expression parse_additive();
    Expression parse_multiplicative();
    Expression parse_unary();
    Expression parse_primary();
};

} // namespace sql::parser
