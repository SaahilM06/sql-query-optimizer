#pragma once

#include <string>
#include <vector>

#include "token.hpp"

namespace sql::parser {

// Throws std::runtime_error on malformed input (unterminated string literal,
// invalid numeric literal, or an unrecognized character) — the equivalent of
// Rust's `Result<_, String>` error path.
class Lexer {
public:
    explicit Lexer(std::string input);

    std::vector<Token> tokenize();
    Token next_token();

private:
    std::vector<char> input_;
    size_t pos_ = 0;

    bool peek(char& out) const;
    char advance();
    void skip_whitespace();
};

} // namespace sql::parser
