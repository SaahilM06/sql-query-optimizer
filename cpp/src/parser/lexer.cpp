#include "lexer.hpp"

#include <cctype>
#include <stdexcept>

namespace sql::parser {

Lexer::Lexer(std::string input) : input_(input.begin(), input.end()) {}

bool Lexer::peek(char& out) const {
    if (pos_ >= input_.size()) return false;
    out = input_[pos_];
    return true;
}

char Lexer::advance() {
    char ch = pos_ < input_.size() ? input_[pos_] : '\0';
    ++pos_;
    return ch;
}

void Lexer::skip_whitespace() {
    char ch;
    while (peek(ch) && std::isspace(static_cast<unsigned char>(ch))) {
        advance();
    }
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    for (;;) {
        Token tok = next_token();
        bool is_eof = tok.kind == TokenKind::Eof;
        tokens.push_back(std::move(tok));
        if (is_eof) break;
    }
    return tokens;
}

Token Lexer::next_token() {
    skip_whitespace();

    char ch;
    if (!peek(ch)) return Token(TokenKind::Eof);

    switch (ch) {
        case ',': advance(); return Token(TokenKind::Comma);
        case '.': advance(); return Token(TokenKind::Dot);
        case '(': advance(); return Token(TokenKind::LParen);
        case ')': advance(); return Token(TokenKind::RParen);
        case '+': advance(); return Token(TokenKind::Plus);
        case '-': advance(); return Token(TokenKind::Minus);
        case '*': advance(); return Token(TokenKind::Star);
        case '/': advance(); return Token(TokenKind::Slash);
        case '=': advance(); return Token(TokenKind::Eq);

        case '<': {
            advance();
            char next;
            if (peek(next) && next == '=') { advance(); return Token(TokenKind::Lte); }
            if (peek(next) && next == '>') { advance(); return Token(TokenKind::Neq); }
            return Token(TokenKind::Lt);
        }
        case '>': {
            advance();
            char next;
            if (peek(next) && next == '=') { advance(); return Token(TokenKind::Gte); }
            return Token(TokenKind::Gt);
        }
        case '!': {
            advance();
            char next;
            if (peek(next) && next == '=') { advance(); return Token(TokenKind::Neq); }
            throw std::runtime_error("expected '=' after '!'");
        }

        case '\'': {
            advance(); // skip opening quote
            std::string s;
            for (;;) {
                char c;
                if (!peek(c)) throw std::runtime_error("unterminated string literal");
                if (c == '\'') { advance(); break; }
                advance();
                s.push_back(c);
            }
            return make_string_lit(std::move(s));
        }

        default:
            break;
    }

    if (std::isdigit(static_cast<unsigned char>(ch))) {
        std::string num;
        char c;
        while (peek(c) && std::isdigit(static_cast<unsigned char>(c))) {
            advance();
            num.push_back(c);
        }
        char maybe_dot;
        if (peek(maybe_dot) && maybe_dot == '.') {
            char after_dot = pos_ + 1 < input_.size() ? input_[pos_ + 1] : '\0';
            if (std::isdigit(static_cast<unsigned char>(after_dot))) {
                advance(); // consume '.'
                num.push_back('.');
                while (peek(c) && std::isdigit(static_cast<unsigned char>(c))) {
                    advance();
                    num.push_back(c);
                }
                try {
                    return make_float(std::stod(num));
                } catch (...) {
                    throw std::runtime_error("invalid float");
                }
            }
        }
        try {
            return make_integer(static_cast<int64_t>(std::stoll(num)));
        } catch (...) {
            throw std::runtime_error("invalid integer");
        }
    }

    if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
        std::string word;
        char c;
        while (peek(c) && (std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            advance();
            word.push_back(c);
        }
        std::string upper = word;
        for (auto& ch2 : upper) ch2 = static_cast<char>(std::toupper(static_cast<unsigned char>(ch2)));

        if (upper == "SELECT") return Token(TokenKind::Select);
        if (upper == "FROM") return Token(TokenKind::From);
        if (upper == "WHERE") return Token(TokenKind::Where);
        if (upper == "JOIN") return Token(TokenKind::Join);
        if (upper == "INNER") return Token(TokenKind::Inner);
        if (upper == "LEFT") return Token(TokenKind::Left);
        if (upper == "RIGHT") return Token(TokenKind::Right);
        if (upper == "CROSS") return Token(TokenKind::Cross);
        if (upper == "ON") return Token(TokenKind::On);
        if (upper == "AND") return Token(TokenKind::And);
        if (upper == "OR") return Token(TokenKind::Or);
        if (upper == "NOT") return Token(TokenKind::Not);
        if (upper == "AS") return Token(TokenKind::As);
        if (upper == "ORDER") return Token(TokenKind::Order);
        if (upper == "BY") return Token(TokenKind::By);
        if (upper == "GROUP") return Token(TokenKind::Group);
        if (upper == "LIMIT") return Token(TokenKind::Limit);
        if (upper == "ASC") return Token(TokenKind::Asc);
        if (upper == "DESC") return Token(TokenKind::Desc);
        if (upper == "HAVING") return Token(TokenKind::Having);
        if (upper == "NULL") return Token(TokenKind::Null);
        if (upper == "TRUE") return Token(TokenKind::True);
        if (upper == "FALSE") return Token(TokenKind::False);
        return make_identifier(std::move(word));
    }

    throw std::runtime_error(std::string("unexpected character: '") + ch + "'");
}

} // namespace sql::parser
