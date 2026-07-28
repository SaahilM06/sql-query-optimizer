#include "parser.hpp"

#include <stdexcept>

namespace sql::parser {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

const Token& Parser::peek() const {
    static const Token eof(TokenKind::Eof);
    return pos_ < tokens_.size() ? tokens_[pos_] : eof;
}

Token Parser::advance() {
    Token tok = pos_ < tokens_.size() ? tokens_[pos_] : Token(TokenKind::Eof);
    if (pos_ < tokens_.size()) ++pos_;
    return tok;
}

void Parser::expect(TokenKind expected) {
    if (peek().kind == expected) {
        advance();
        return;
    }
    throw std::runtime_error("unexpected token during parse");
}

// ── Top-level entry point ────────────────────────────────────────────────────

Statement Parser::parse() {
    if (peek().kind == TokenKind::Select) return parse_select();
    throw std::runtime_error("expected SELECT");
}

// ── SELECT statement ──────────────────────────────────────────────────────────

Statement Parser::parse_select() {
    advance(); // consume SELECT

    SelectStatement stmt;
    stmt.columns = parse_select_list();

    expect(TokenKind::From);
    stmt.from = parse_table_ref();

    // Zero or more JOINs
    while (peek().kind == TokenKind::Join || peek().kind == TokenKind::Inner ||
           peek().kind == TokenKind::Left || peek().kind == TokenKind::Right ||
           peek().kind == TokenKind::Cross) {
        stmt.joins.push_back(parse_join());
    }

    if (peek().kind == TokenKind::Where) {
        advance();
        stmt.where_clause = parse_expr();
    }

    if (peek().kind == TokenKind::Group) {
        advance();
        expect(TokenKind::By);
        stmt.group_by = parse_expr_list();
    }

    if (peek().kind == TokenKind::Having) {
        advance();
        stmt.having = parse_expr();
    }

    if (peek().kind == TokenKind::Order) {
        advance();
        expect(TokenKind::By);
        stmt.order_by = parse_order_by_list();
    }

    if (peek().kind == TokenKind::Limit) {
        advance();
        Token tok = advance();
        if (tok.kind != TokenKind::Integer) {
            throw std::runtime_error("expected integer after LIMIT");
        }
        stmt.limit = static_cast<size_t>(tok.integer());
    }

    return Statement::make_select(std::move(stmt));
}

// ── SELECT list ───────────────────────────────────────────────────────────────

std::vector<SelectItem> Parser::parse_select_list() {
    std::vector<SelectItem> items;
    for (;;) {
        if (peek().kind == TokenKind::Star) {
            advance();
            items.push_back(SelectItem::make_wildcard());
        } else {
            Expression expr = parse_expr();

            // `table.*` is emitted by parse_primary as Column{table: Some(t), column: "*"}.
            // Convert it to QualifiedWildcard here in the SELECT-list context.
            if (expr.kind == Expression::Kind::Column && expr.table.has_value() && expr.column == "*") {
                items.push_back(SelectItem::make_qualified_wildcard(*expr.table));
            } else {
                std::optional<std::string> alias;
                if (peek().kind == TokenKind::As) {
                    advance();
                    Token tok = advance();
                    if (tok.kind != TokenKind::Identifier) {
                        throw std::runtime_error("expected alias name after AS");
                    }
                    alias = tok.text();
                } else if (peek().kind == TokenKind::Identifier) {
                    // Implicit alias: `expr name`
                    alias = advance().text();
                }
                items.push_back(SelectItem::make_expression(std::move(expr), std::move(alias)));
            }
        }

        if (peek().kind == TokenKind::Comma) {
            advance();
        } else {
            break;
        }
    }
    return items;
}

// ── Table reference ───────────────────────────────────────────────────────────

TableRef Parser::parse_table_ref() {
    Token name_tok = advance();
    if (name_tok.kind != TokenKind::Identifier) {
        throw std::runtime_error("expected table name");
    }

    TableRef ref;
    ref.table_name = name_tok.text();

    if (peek().kind == TokenKind::As) {
        advance();
        Token tok = advance();
        if (tok.kind != TokenKind::Identifier) throw std::runtime_error("expected alias after AS");
        ref.alias = tok.text();
    } else if (peek().kind == TokenKind::Identifier) {
        ref.alias = advance().text();
    }

    return ref;
}

// ── JOIN clause ────────────────────────────────────────────────────────────────

Join Parser::parse_join() {
    JoinType join_type;
    switch (peek().kind) {
        case TokenKind::Join:
            advance();
            join_type = JoinType::Inner;
            break;
        case TokenKind::Inner:
            advance();
            expect(TokenKind::Join);
            join_type = JoinType::Inner;
            break;
        case TokenKind::Left:
            advance();
            expect(TokenKind::Join);
            join_type = JoinType::Left;
            break;
        case TokenKind::Right:
            advance();
            expect(TokenKind::Join);
            join_type = JoinType::Right;
            break;
        case TokenKind::Cross:
            advance();
            expect(TokenKind::Join);
            join_type = JoinType::Cross;
            break;
        default:
            throw std::runtime_error("expected JOIN keyword");
    }

    Join join;
    join.join_type = join_type;
    join.table = parse_table_ref();
    expect(TokenKind::On);
    join.condition = parse_expr();
    return join;
}

// ── ORDER BY list ──────────────────────────────────────────────────────────────

std::vector<OrderByItem> Parser::parse_order_by_list() {
    std::vector<OrderByItem> items;
    for (;;) {
        Expression expr = parse_expr();
        bool ascending = true;
        if (peek().kind == TokenKind::Asc) {
            advance();
            ascending = true;
        } else if (peek().kind == TokenKind::Desc) {
            advance();
            ascending = false;
        }
        items.push_back(OrderByItem{std::move(expr), ascending});

        if (peek().kind == TokenKind::Comma) {
            advance();
        } else {
            break;
        }
    }
    return items;
}

// ── Comma-separated expression list ───────────────────────────────────────────

std::vector<Expression> Parser::parse_expr_list() {
    std::vector<Expression> exprs;
    for (;;) {
        exprs.push_back(parse_expr());
        if (peek().kind == TokenKind::Comma) {
            advance();
        } else {
            break;
        }
    }
    return exprs;
}

// ── Expression parsing (recursive descent by precedence) ──────────────────────

Expression Parser::parse_expr() { return parse_or(); }

Expression Parser::parse_or() {
    Expression left = parse_and();
    while (peek().kind == TokenKind::Or) {
        advance();
        Expression right = parse_and();
        left = Expression::make_binary_op(std::move(left), BinaryOperator::Or, std::move(right));
    }
    return left;
}

Expression Parser::parse_and() {
    Expression left = parse_not();
    while (peek().kind == TokenKind::And) {
        advance();
        Expression right = parse_not();
        left = Expression::make_binary_op(std::move(left), BinaryOperator::And, std::move(right));
    }
    return left;
}

Expression Parser::parse_not() {
    if (peek().kind == TokenKind::Not) {
        advance();
        Expression expr = parse_not(); // right-associative: NOT NOT a => Not(Not(a))
        return Expression::make_unary_op(UnaryOperator::Not, std::move(expr));
    }
    return parse_comparison();
}

Expression Parser::parse_comparison() {
    Expression left = parse_additive();
    BinaryOperator op;
    switch (peek().kind) {
        case TokenKind::Eq:  op = BinaryOperator::Eq;  break;
        case TokenKind::Neq: op = BinaryOperator::Neq; break;
        case TokenKind::Lt:  op = BinaryOperator::Lt;  break;
        case TokenKind::Gt:  op = BinaryOperator::Gt;  break;
        case TokenKind::Lte: op = BinaryOperator::Lte; break;
        case TokenKind::Gte: op = BinaryOperator::Gte; break;
        default: return left;
    }
    advance();
    Expression right = parse_additive();
    return Expression::make_binary_op(std::move(left), op, std::move(right));
}

Expression Parser::parse_additive() {
    Expression left = parse_multiplicative();
    for (;;) {
        BinaryOperator op;
        if (peek().kind == TokenKind::Plus) op = BinaryOperator::Add;
        else if (peek().kind == TokenKind::Minus) op = BinaryOperator::Sub;
        else break;
        advance();
        Expression right = parse_multiplicative();
        left = Expression::make_binary_op(std::move(left), op, std::move(right));
    }
    return left;
}

Expression Parser::parse_multiplicative() {
    Expression left = parse_unary();
    for (;;) {
        BinaryOperator op;
        if (peek().kind == TokenKind::Star) op = BinaryOperator::Mul;
        else if (peek().kind == TokenKind::Slash) op = BinaryOperator::Div;
        else break;
        advance();
        Expression right = parse_unary();
        left = Expression::make_binary_op(std::move(left), op, std::move(right));
    }
    return left;
}

Expression Parser::parse_unary() {
    if (peek().kind == TokenKind::Minus) {
        advance();
        Expression expr = parse_unary(); // right-associative
        return Expression::make_unary_op(UnaryOperator::Neg, std::move(expr));
    }
    return parse_primary();
}

Expression Parser::parse_primary() {
    const Token tok = peek();

    switch (tok.kind) {
        case TokenKind::LParen: {
            advance();
            Expression expr = parse_expr();
            expect(TokenKind::RParen);
            return expr;
        }
        case TokenKind::Integer:
            advance();
            return Expression::make_literal(Literal::integer(tok.integer()));
        case TokenKind::Float:
            advance();
            return Expression::make_literal(Literal::floating(tok.floating()));
        case TokenKind::StringLit:
            advance();
            return Expression::make_literal(Literal::str(tok.text()));
        case TokenKind::True:
            advance();
            return Expression::make_literal(Literal::boolean(true));
        case TokenKind::False:
            advance();
            return Expression::make_literal(Literal::boolean(false));
        case TokenKind::Null:
            advance();
            return Expression::make_literal(Literal::null());

        case TokenKind::Identifier: {
            std::string name = tok.text();
            advance();

            // Function call: name(...)
            if (peek().kind == TokenKind::LParen) {
                advance(); // consume '('
                std::vector<Expression> args;
                if (peek().kind == TokenKind::RParen) {
                    // no args
                } else if (peek().kind == TokenKind::Star) {
                    // COUNT(*) — consume '*', yield Wildcard arg
                    advance();
                    args.push_back(Expression::make_wildcard());
                } else {
                    args = parse_expr_list();
                }
                expect(TokenKind::RParen);
                return Expression::make_function(std::move(name), std::move(args));
            }

            // Qualified: table.column or table.*
            if (peek().kind == TokenKind::Dot) {
                advance(); // consume '.'
                Token next = advance();
                if (next.kind == TokenKind::Identifier) {
                    return Expression::make_column(std::move(name), next.text());
                }
                if (next.kind == TokenKind::Star) {
                    // table.* — represented as Column{table, column: "*"} so
                    // parse_select_list can convert it to QualifiedWildcard.
                    return Expression::make_column(std::move(name), "*");
                }
                throw std::runtime_error("expected column name after '.'");
            }

            return Expression::make_column(std::nullopt, std::move(name));
        }

        default:
            throw std::runtime_error("unexpected token in expression");
    }
}

} // namespace sql::parser
