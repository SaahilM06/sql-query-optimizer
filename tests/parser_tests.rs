// Integration tests for the SQL query optimizer parser layer.
// These live in tests/ so they compile as a separate crate that imports
// sql_query_optimizer as a library — exactly how an external user would.

use sql_query_optimizer::parser::ast::*;
use sql_query_optimizer::parser::lexer::Lexer;
use sql_query_optimizer::parser::parser::Parser;

// ── helpers ──────────────────────────────────────────────────────────────────

fn parse(sql: &str) -> Statement {
    let tokens = Lexer::new(sql).tokenize().expect("lex error");
    Parser::new(tokens).parse().expect("parse error")
}

fn parse_err(sql: &str) -> String {
    let tokens = Lexer::new(sql).tokenize().expect("lex error");
    Parser::new(tokens).parse().expect_err("expected a parse error but got Ok")
}

fn select(sql: &str) -> SelectStatement {
    match parse(sql) {
        Statement::Select(s) => s,
    }
}

fn where_expr(sql: &str) -> Expression {
    select(sql).where_clause.expect("no WHERE clause")
}

// ── Lexer tests ───────────────────────────────────────────────────────────────

#[test]
fn lex_keywords_are_case_insensitive() {
    // "select" and "SELECT" should produce the same token stream
    let lower = Lexer::new("select * from t").tokenize().unwrap();
    let upper = Lexer::new("SELECT * FROM t").tokenize().unwrap();
    assert_eq!(lower, upper);
}

#[test]
fn lex_string_literal() {
    let tokens = Lexer::new("'hello world'").tokenize().unwrap();
    use sql_query_optimizer::parser::lexer::Token;
    assert!(matches!(&tokens[0], Token::StringLit(s) if s == "hello world"));
}

#[test]
fn lex_float_vs_integer() {
    use sql_query_optimizer::parser::lexer::Token;
    let tokens = Lexer::new("42 3.14").tokenize().unwrap();
    assert!(matches!(tokens[0], Token::Integer(42)));
    assert!(matches!(tokens[1], Token::Float(f) if (f - 3.14).abs() < 1e-9));
}

#[test]
fn lex_all_comparison_operators() {
    use sql_query_optimizer::parser::lexer::Token;
    let tokens = Lexer::new("= <> != < > <= >=").tokenize().unwrap();
    // Token::Eof at the end; 7 operators expected before it
    assert_eq!(tokens.len(), 8);
    assert!(matches!(tokens[0], Token::Eq));
    assert!(matches!(tokens[1], Token::Neq));
    assert!(matches!(tokens[2], Token::Neq)); // != is an alias for <>
    assert!(matches!(tokens[3], Token::Lt));
    assert!(matches!(tokens[4], Token::Gt));
    assert!(matches!(tokens[5], Token::Lte));
    assert!(matches!(tokens[6], Token::Gte));
}

// ── Parser — structural tests ─────────────────────────────────────────────────

#[test]
fn parse_select_star() {
    let s = select("SELECT * FROM orders");
    assert!(matches!(s.columns[0], SelectItem::Wildcard));
    assert_eq!(s.from.table_name, "orders");
    assert!(s.joins.is_empty());
    assert!(s.where_clause.is_none());
}

#[test]
fn parse_table_alias() {
    let s = select("SELECT * FROM customers c");
    assert_eq!(s.from.alias.as_deref(), Some("c"));
}

#[test]
fn parse_column_alias_with_as() {
    let s = select("SELECT price AS p FROM products");
    if let SelectItem::Expression(_, alias) = &s.columns[0] {
        assert_eq!(alias.as_deref(), Some("p"));
    } else {
        panic!("expected SelectItem::Expression");
    }
}

#[test]
fn parse_multiple_columns() {
    let s = select("SELECT id, name, email FROM users");
    assert_eq!(s.columns.len(), 3);
}

#[test]
fn parse_qualified_wildcard() {
    // t.* should become SelectItem::QualifiedWildcard("t")
    let s = select("SELECT t.* FROM t");
    assert!(
        matches!(&s.columns[0], SelectItem::QualifiedWildcard(tbl) if tbl == "t"),
        "expected QualifiedWildcard"
    );
}

#[test]
fn parse_left_join() {
    let s = select("SELECT * FROM a LEFT JOIN b ON a.id = b.a_id");
    assert_eq!(s.joins[0].join_type, JoinType::Left);
    assert_eq!(s.joins[0].table.table_name, "b");
}

#[test]
fn parse_right_join() {
    let s = select("SELECT * FROM a RIGHT JOIN b ON a.id = b.a_id");
    assert_eq!(s.joins[0].join_type, JoinType::Right);
}

#[test]
fn parse_cross_join() {
    let s = select("SELECT * FROM a CROSS JOIN b ON a.id = b.id");
    assert_eq!(s.joins[0].join_type, JoinType::Cross);
}

#[test]
fn parse_multiple_joins() {
    let sql = "SELECT * FROM a \
               INNER JOIN b ON a.id = b.a_id \
               LEFT JOIN c ON b.id = c.b_id";
    let s = select(sql);
    assert_eq!(s.joins.len(), 2);
    assert_eq!(s.joins[0].join_type, JoinType::Inner);
    assert_eq!(s.joins[1].join_type, JoinType::Left);
}

#[test]
fn parse_where_simple_equality() {
    let expr = where_expr("SELECT * FROM t WHERE status = 'active'");
    assert!(matches!(expr, Expression::BinaryOp { op: BinaryOperator::Eq, .. }));
}

#[test]
fn parse_group_by_multiple_columns() {
    let s = select("SELECT dept, role, COUNT(*) FROM emp GROUP BY dept, role");
    assert_eq!(s.group_by.len(), 2);
}

#[test]
fn parse_order_by_asc_default() {
    // Without ASC/DESC, should default to ascending
    let s = select("SELECT id FROM t ORDER BY id");
    assert!(s.order_by[0].ascending);
}

#[test]
fn parse_order_by_desc() {
    let s = select("SELECT id FROM t ORDER BY id DESC");
    assert!(!s.order_by[0].ascending);
}

#[test]
fn parse_limit() {
    let s = select("SELECT * FROM t LIMIT 25");
    assert_eq!(s.limit, Some(25));
}

#[test]
fn parse_no_limit_is_none() {
    let s = select("SELECT * FROM t");
    assert_eq!(s.limit, None);
}

#[test]
fn parse_function_avg() {
    let s = select("SELECT AVG(price) FROM products");
    if let SelectItem::Expression(Expression::Function { name, args }, _) = &s.columns[0] {
        assert_eq!(name, "AVG");
        assert_eq!(args.len(), 1);
    } else {
        panic!("expected Function expression");
    }
}

#[test]
fn parse_count_star() {
    let s = select("SELECT COUNT(*) FROM t");
    if let SelectItem::Expression(Expression::Function { name, args }, _) = &s.columns[0] {
        assert_eq!(name, "COUNT");
        assert!(matches!(args[0], Expression::Wildcard));
    } else {
        panic!("expected COUNT(*) function");
    }
}

// ── Parser — literal types ─────────────────────────────────────────────────────

#[test]
fn parse_integer_literal_in_where() {
    let expr = where_expr("SELECT 1 FROM t WHERE id = 99");
    if let Expression::BinaryOp { right, .. } = expr {
        assert!(matches!(*right, Expression::Literal(Literal::Integer(99))));
    }
}

#[test]
fn parse_string_literal_in_where() {
    let expr = where_expr("SELECT 1 FROM t WHERE name = 'alice'");
    if let Expression::BinaryOp { right, .. } = expr {
        assert!(matches!(*right, Expression::Literal(Literal::Str(ref s)) if s == "alice"));
    }
}

#[test]
fn parse_boolean_true() {
    let expr = where_expr("SELECT 1 FROM t WHERE active = TRUE");
    if let Expression::BinaryOp { right, .. } = expr {
        assert!(matches!(*right, Expression::Literal(Literal::Boolean(true))));
    }
}

#[test]
fn parse_null_literal() {
    let expr = where_expr("SELECT 1 FROM t WHERE deleted = NULL");
    if let Expression::BinaryOp { right, .. } = expr {
        assert!(matches!(*right, Expression::Literal(Literal::Null)));
    }
}

// ── Parser — expression precedence ────────────────────────────────────────────

#[test]
fn and_binds_tighter_than_or() {
    // a=1 OR b=2 AND c=3  =>  OR(a=1, AND(b=2, c=3))
    let expr = where_expr("SELECT 1 FROM t WHERE a = 1 OR b = 2 AND c = 3");
    assert!(
        matches!(expr, Expression::BinaryOp { op: BinaryOperator::Or, .. }),
        "top-level must be OR"
    );
}

#[test]
fn mul_binds_tighter_than_add() {
    // a + b*c = 0  =>  Eq(Add(a, Mul(b,c)), 0)
    let expr = where_expr("SELECT 1 FROM t WHERE a + b * c = 0");
    if let Expression::BinaryOp { op: BinaryOperator::Eq, left, .. } = expr {
        assert!(matches!(*left, Expression::BinaryOp { op: BinaryOperator::Add, .. }));
    }
}

#[test]
fn parens_override_precedence() {
    // (a+b)*c = 0  =>  Eq(Mul(Add(a,b),c), 0)
    let expr = where_expr("SELECT 1 FROM t WHERE (a + b) * c = 0");
    if let Expression::BinaryOp { op: BinaryOperator::Eq, left, .. } = expr {
        assert!(matches!(*left, Expression::BinaryOp { op: BinaryOperator::Mul, .. }));
    }
}

#[test]
fn not_binds_tighter_than_and() {
    // NOT a=1 AND b=2  =>  AND(NOT(a=1), b=2)
    let expr = where_expr("SELECT 1 FROM t WHERE NOT a = 1 AND b = 2");
    if let Expression::BinaryOp { op: BinaryOperator::And, left, .. } = expr {
        assert!(matches!(*left, Expression::UnaryOp { op: UnaryOperator::Not, .. }));
    }
}

#[test]
fn unary_minus_precedence() {
    // -a + b = 0  =>  Eq(Add(Neg(a), b), 0)
    let expr = where_expr("SELECT 1 FROM t WHERE -a + b = 0");
    if let Expression::BinaryOp { op: BinaryOperator::Eq, left, .. } = expr {
        if let Expression::BinaryOp { op: BinaryOperator::Add, left: neg, .. } = *left {
            assert!(matches!(*neg, Expression::UnaryOp { op: UnaryOperator::Neg, .. }));
        }
    }
}

#[test]
fn chained_and() {
    // a=1 AND b=2 AND c=3 is left-associative: AND(AND(a=1,b=2), c=3)
    let expr = where_expr("SELECT 1 FROM t WHERE a = 1 AND b = 2 AND c = 3");
    if let Expression::BinaryOp { op: BinaryOperator::And, left, .. } = expr {
        assert!(matches!(*left, Expression::BinaryOp { op: BinaryOperator::And, .. }));
    } else {
        panic!("expected AND at top");
    }
}

// ── Print / inspect tests  (run with: cargo test print_ -- --nocapture) ────────

#[test]
fn print_ast_group_by_sum() {
    let sql = "SELECT c.name, SUM(o.total) \
               FROM customers c \
               JOIN orders o ON c.id = o.customer_id \
               GROUP BY c.name";

    let stmt = parse(sql);
    println!("SQL: {}", sql);
    println!("AST: {:#?}", stmt);
}

// ── Parser — error cases ───────────────────────────────────────────────────────

#[test]
fn error_on_missing_from() {
    let err = parse_err("SELECT *");
    assert!(!err.is_empty(), "should return an error message");
}

#[test]
fn error_on_empty_input() {
    let err = parse_err("");
    assert!(!err.is_empty());
}

#[test]
fn error_on_non_select() {
    // Only SELECT is supported so far; INSERT should fail
    let err = parse_err("INSERT INTO t VALUES (1)");
    assert!(!err.is_empty());
}
