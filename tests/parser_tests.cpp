// Tests for the SQL query optimizer parser layer (lexer + recursive-descent
// parser). Ported 1:1 from the original Rust `tests/parser_tests.rs`.

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "../parser/ast.hpp"
#include "../parser/lexer.hpp"
#include "../parser/parser.hpp"
#include "test_framework.hpp"

using namespace sql::parser;

namespace {

// ── helpers ──────────────────────────────────────────────────────────────────

Statement parse(const std::string& sql) {
    Lexer lexer(sql);
    std::vector<Token> tokens = lexer.tokenize();
    Parser parser(std::move(tokens));
    return parser.parse();
}

std::string parse_err(const std::string& sql) {
    Lexer lexer(sql);
    std::vector<Token> tokens;
    try {
        tokens = lexer.tokenize();
    } catch (const std::exception& e) {
        FAIL_TEST(std::string("lex error: ") + e.what());
    }
    Parser parser(std::move(tokens));
    try {
        parser.parse();
    } catch (const std::exception& e) {
        return e.what();
    }
    FAIL_TEST("expected a parse error but got Ok");
    return "";
}

SelectStatement select(const std::string& sql) {
    Statement stmt = parse(sql);
    return std::move(stmt.select);
}

Expression where_expr(const std::string& sql) {
    SelectStatement s = select(sql);
    ASSERT_TRUE_MSG(s.where_clause.has_value(), "no WHERE clause");
    return std::move(*s.where_clause);
}

} // namespace

// ── Lexer tests ────────────────────────────────────────────────────────────────

TEST(lex_keywords_are_case_insensitive) {
    // "select" and "SELECT" should produce the same token stream
    Lexer lower_lexer("select * from t");
    Lexer upper_lexer("SELECT * FROM t");
    auto lower = lower_lexer.tokenize();
    auto upper = upper_lexer.tokenize();
    ASSERT_TRUE(lower == upper);
}

TEST(lex_string_literal) {
    Lexer lexer("'hello world'");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens[0].kind == TokenKind::StringLit);
    ASSERT_EQ(tokens[0].text(), std::string("hello world"));
}

TEST(lex_float_vs_integer) {
    Lexer lexer("42 3.14");
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(tokens[0].kind == TokenKind::Integer);
    ASSERT_EQ(tokens[0].integer(), static_cast<int64_t>(42));
    ASSERT_TRUE(tokens[1].kind == TokenKind::Float);
    ASSERT_TRUE(std::abs(tokens[1].floating() - 3.14) < 1e-9);
}

TEST(lex_all_comparison_operators) {
    Lexer lexer("= <> != < > <= >=");
    auto tokens = lexer.tokenize();
    // Token::Eof at the end; 7 operators expected before it
    ASSERT_EQ(tokens.size(), static_cast<size_t>(8));
    ASSERT_TRUE(tokens[0].kind == TokenKind::Eq);
    ASSERT_TRUE(tokens[1].kind == TokenKind::Neq);
    ASSERT_TRUE(tokens[2].kind == TokenKind::Neq); // != is an alias for <>
    ASSERT_TRUE(tokens[3].kind == TokenKind::Lt);
    ASSERT_TRUE(tokens[4].kind == TokenKind::Gt);
    ASSERT_TRUE(tokens[5].kind == TokenKind::Lte);
    ASSERT_TRUE(tokens[6].kind == TokenKind::Gte);
}

// ── Parser -- structural tests ───────────────────────────────────────────────

TEST(parse_select_star) {
    auto s = select("SELECT * FROM orders");
    ASSERT_TRUE(s.columns[0].kind == SelectItem::Kind::Wildcard);
    ASSERT_EQ(s.from.table_name, std::string("orders"));
    ASSERT_TRUE(s.joins.empty());
    ASSERT_FALSE(s.where_clause.has_value());
}

TEST(parse_table_alias) {
    auto s = select("SELECT * FROM customers c");
    ASSERT_TRUE(s.from.alias.has_value());
    ASSERT_EQ(*s.from.alias, std::string("c"));
}

TEST(parse_column_alias_with_as) {
    auto s = select("SELECT price AS p FROM products");
    ASSERT_TRUE(s.columns[0].kind == SelectItem::Kind::Expression);
    ASSERT_TRUE(s.columns[0].alias.has_value());
    ASSERT_EQ(*s.columns[0].alias, std::string("p"));
}

TEST(parse_multiple_columns) {
    auto s = select("SELECT id, name, email FROM users");
    ASSERT_EQ(s.columns.size(), static_cast<size_t>(3));
}

TEST(parse_qualified_wildcard) {
    // t.* should become SelectItem::QualifiedWildcard("t")
    auto s = select("SELECT t.* FROM t");
    ASSERT_TRUE_MSG(s.columns[0].kind == SelectItem::Kind::QualifiedWildcard, "expected QualifiedWildcard");
    ASSERT_EQ(s.columns[0].qualified_table, std::string("t"));
}

TEST(parse_left_join) {
    auto s = select("SELECT * FROM a LEFT JOIN b ON a.id = b.a_id");
    ASSERT_TRUE(s.joins[0].join_type == JoinType::Left);
    ASSERT_EQ(s.joins[0].table.table_name, std::string("b"));
}

TEST(parse_right_join) {
    auto s = select("SELECT * FROM a RIGHT JOIN b ON a.id = b.a_id");
    ASSERT_TRUE(s.joins[0].join_type == JoinType::Right);
}

TEST(parse_cross_join) {
    auto s = select("SELECT * FROM a CROSS JOIN b ON a.id = b.id");
    ASSERT_TRUE(s.joins[0].join_type == JoinType::Cross);
}

TEST(parse_multiple_joins) {
    std::string sql = "SELECT * FROM a "
                       "INNER JOIN b ON a.id = b.a_id "
                       "LEFT JOIN c ON b.id = c.b_id";
    auto s = select(sql);
    ASSERT_EQ(s.joins.size(), static_cast<size_t>(2));
    ASSERT_TRUE(s.joins[0].join_type == JoinType::Inner);
    ASSERT_TRUE(s.joins[1].join_type == JoinType::Left);
}

TEST(parse_where_simple_equality) {
    auto expr = where_expr("SELECT * FROM t WHERE status = 'active'");
    ASSERT_TRUE(expr.kind == Expression::Kind::BinaryOp);
    ASSERT_TRUE(expr.binary_op == BinaryOperator::Eq);
}

TEST(parse_group_by_multiple_columns) {
    auto s = select("SELECT dept, role, COUNT(*) FROM emp GROUP BY dept, role");
    ASSERT_EQ(s.group_by.size(), static_cast<size_t>(2));
}

TEST(parse_order_by_asc_default) {
    // Without ASC/DESC, should default to ascending
    auto s = select("SELECT id FROM t ORDER BY id");
    ASSERT_TRUE(s.order_by[0].ascending);
}

TEST(parse_order_by_desc) {
    auto s = select("SELECT id FROM t ORDER BY id DESC");
    ASSERT_FALSE(s.order_by[0].ascending);
}

TEST(parse_limit) {
    auto s = select("SELECT * FROM t LIMIT 25");
    ASSERT_TRUE(s.limit.has_value());
    ASSERT_EQ(*s.limit, static_cast<size_t>(25));
}

TEST(parse_no_limit_is_none) {
    auto s = select("SELECT * FROM t");
    ASSERT_FALSE(s.limit.has_value());
}

TEST(parse_function_avg) {
    auto s = select("SELECT AVG(price) FROM products");
    ASSERT_TRUE(s.columns[0].kind == SelectItem::Kind::Expression);
    const Expression& e = *s.columns[0].expr;
    ASSERT_TRUE(e.kind == Expression::Kind::Function);
    ASSERT_EQ(e.func_name, std::string("AVG"));
    ASSERT_EQ(e.args.size(), static_cast<size_t>(1));
}

TEST(parse_count_star) {
    auto s = select("SELECT COUNT(*) FROM t");
    ASSERT_TRUE(s.columns[0].kind == SelectItem::Kind::Expression);
    const Expression& e = *s.columns[0].expr;
    ASSERT_TRUE(e.kind == Expression::Kind::Function);
    ASSERT_EQ(e.func_name, std::string("COUNT"));
    ASSERT_TRUE(e.args[0].kind == Expression::Kind::Wildcard);
}

// ── Parser -- literal types ───────────────────────────────────────────────────

TEST(parse_integer_literal_in_where) {
    auto expr = where_expr("SELECT 1 FROM t WHERE id = 99");
    ASSERT_TRUE(expr.kind == Expression::Kind::BinaryOp);
    ASSERT_TRUE(expr.right->kind == Expression::Kind::Literal);
    ASSERT_TRUE(expr.right->literal.kind == Literal::Kind::Integer);
    ASSERT_EQ(expr.right->literal.int_val, static_cast<int64_t>(99));
}

TEST(parse_string_literal_in_where) {
    auto expr = where_expr("SELECT 1 FROM t WHERE name = 'alice'");
    ASSERT_TRUE(expr.right->kind == Expression::Kind::Literal);
    ASSERT_TRUE(expr.right->literal.kind == Literal::Kind::Str);
    ASSERT_EQ(expr.right->literal.str_val, std::string("alice"));
}

TEST(parse_boolean_true) {
    auto expr = where_expr("SELECT 1 FROM t WHERE active = TRUE");
    ASSERT_TRUE(expr.right->kind == Expression::Kind::Literal);
    ASSERT_TRUE(expr.right->literal.kind == Literal::Kind::Boolean);
    ASSERT_TRUE(expr.right->literal.bool_val);
}

TEST(parse_null_literal) {
    auto expr = where_expr("SELECT 1 FROM t WHERE deleted = NULL");
    ASSERT_TRUE(expr.right->kind == Expression::Kind::Literal);
    ASSERT_TRUE(expr.right->literal.kind == Literal::Kind::Null);
}

// ── Parser -- expression precedence ──────────────────────────────────────────

TEST(and_binds_tighter_than_or) {
    // a=1 OR b=2 AND c=3  =>  OR(a=1, AND(b=2, c=3))
    auto expr = where_expr("SELECT 1 FROM t WHERE a = 1 OR b = 2 AND c = 3");
    ASSERT_TRUE_MSG(expr.kind == Expression::Kind::BinaryOp && expr.binary_op == BinaryOperator::Or,
                     "top-level must be OR");
}

TEST(mul_binds_tighter_than_add) {
    // a + b*c = 0  =>  Eq(Add(a, Mul(b,c)), 0)
    auto expr = where_expr("SELECT 1 FROM t WHERE a + b * c = 0");
    ASSERT_TRUE(expr.kind == Expression::Kind::BinaryOp && expr.binary_op == BinaryOperator::Eq);
    ASSERT_TRUE(expr.left->kind == Expression::Kind::BinaryOp && expr.left->binary_op == BinaryOperator::Add);
}

TEST(parens_override_precedence) {
    // (a+b)*c = 0  =>  Eq(Mul(Add(a,b),c), 0)
    auto expr = where_expr("SELECT 1 FROM t WHERE (a + b) * c = 0");
    ASSERT_TRUE(expr.kind == Expression::Kind::BinaryOp && expr.binary_op == BinaryOperator::Eq);
    ASSERT_TRUE(expr.left->kind == Expression::Kind::BinaryOp && expr.left->binary_op == BinaryOperator::Mul);
}

TEST(not_binds_tighter_than_and) {
    // NOT a=1 AND b=2  =>  AND(NOT(a=1), b=2)
    auto expr = where_expr("SELECT 1 FROM t WHERE NOT a = 1 AND b = 2");
    ASSERT_TRUE(expr.kind == Expression::Kind::BinaryOp && expr.binary_op == BinaryOperator::And);
    ASSERT_TRUE(expr.left->kind == Expression::Kind::UnaryOp && expr.left->unary_op == UnaryOperator::Not);
}

TEST(unary_minus_precedence) {
    // -a + b = 0  =>  Eq(Add(Neg(a), b), 0)
    auto expr = where_expr("SELECT 1 FROM t WHERE -a + b = 0");
    ASSERT_TRUE(expr.kind == Expression::Kind::BinaryOp && expr.binary_op == BinaryOperator::Eq);
    const Expression& add = *expr.left;
    ASSERT_TRUE(add.kind == Expression::Kind::BinaryOp && add.binary_op == BinaryOperator::Add);
    ASSERT_TRUE(add.left->kind == Expression::Kind::UnaryOp && add.left->unary_op == UnaryOperator::Neg);
}

TEST(chained_and) {
    // a=1 AND b=2 AND c=3 is left-associative: AND(AND(a=1,b=2), c=3)
    auto expr = where_expr("SELECT 1 FROM t WHERE a = 1 AND b = 2 AND c = 3");
    ASSERT_TRUE_MSG(expr.kind == Expression::Kind::BinaryOp && expr.binary_op == BinaryOperator::And,
                     "expected AND at top");
    ASSERT_TRUE(expr.left->kind == Expression::Kind::BinaryOp && expr.left->binary_op == BinaryOperator::And);
}

// ── Print / inspect (smoke test) ──────────────────────────────────────────────

TEST(print_ast_group_by_sum) {
    std::string sql = "SELECT c.name, SUM(o.total) "
                       "FROM customers c "
                       "JOIN orders o ON c.id = o.customer_id "
                       "GROUP BY c.name";
    // Just confirm this parses without throwing.
    Statement stmt = parse(sql);
    (void)stmt;
}

// ── Parser -- error cases ─────────────────────────────────────────────────────

TEST(error_on_missing_from) {
    auto err = parse_err("SELECT *");
    ASSERT_TRUE_MSG(!err.empty(), "should return an error message");
}

TEST(error_on_empty_input) {
    auto err = parse_err("");
    ASSERT_FALSE(err.empty());
}

TEST(error_on_non_select) {
    // Only SELECT is supported so far; INSERT should fail
    auto err = parse_err("INSERT INTO t VALUES (1)");
    ASSERT_FALSE(err.empty());
}
