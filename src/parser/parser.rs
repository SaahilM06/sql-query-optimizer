use crate::parser::ast::*;
use crate::parser::lexer::Token;

pub struct Parser {
    tokens: Vec<Token>,
    pos: usize,
}

impl Parser {
    pub fn new(tokens: Vec<Token>) -> Self {
        Parser { tokens, pos: 0 }
    }

    fn peek(&self) -> &Token {
        self.tokens.get(self.pos).unwrap_or(&Token::Eof)
    }

    /// Consume and return the current token.
    fn advance(&mut self) -> Token {
        let tok = self.tokens.get(self.pos).cloned().unwrap_or(Token::Eof);
        if self.pos < self.tokens.len() {
            self.pos += 1;
        }
        tok
    }

    fn expect(&mut self, expected: &Token) -> Result<(), String> {
        if self.peek() == expected {
            self.advance();
            Ok(())
        } else {
            Err(format!("expected {:?}, got {:?}", expected, self.peek()))
        }
    }

    // ── Top-level entry point ────────────────────────────────────────────────

    pub fn parse(&mut self) -> Result<Statement, String> {
        match self.peek() {
            Token::Select => self.parse_select(),
            _ => {
                let tok = self.peek().clone();
                Err(format!("expected SELECT, got {:?}", tok))
            }
        }
    }

    // ── SELECT statement ─────────────────────────────────────────────────────

    fn parse_select(&mut self) -> Result<Statement, String> {
        self.advance(); // consume SELECT

        let columns = self.parse_select_list()?;

        self.expect(&Token::From)?;
        let from = self.parse_table_ref()?;

        // Zero or more JOINs
        let mut joins = Vec::new();
        while matches!(
            self.peek(),
            Token::Join | Token::Inner | Token::Left | Token::Right | Token::Cross
        ) {
            joins.push(self.parse_join()?);
        }

        let where_clause = if self.peek() == &Token::Where {
            self.advance();
            Some(self.parse_expr()?)
        } else {
            None
        };

        let group_by = if self.peek() == &Token::Group {
            self.advance();
            self.expect(&Token::By)?;
            self.parse_expr_list()?
        } else {
            Vec::new()
        };

        let having = if self.peek() == &Token::Having {
            self.advance();
            Some(self.parse_expr()?)
        } else {
            None
        };

        let order_by = if self.peek() == &Token::Order {
            self.advance();
            self.expect(&Token::By)?;
            self.parse_order_by_list()?
        } else {
            Vec::new()
        };

        let limit = if self.peek() == &Token::Limit {
            self.advance();
            match self.advance() {
                Token::Integer(n) => Some(n as usize),
                tok => return Err(format!("expected integer after LIMIT, got {:?}", tok)),
            }
        } else {
            None
        };

        Ok(Statement::Select(SelectStatement {
            columns,
            from,
            joins,
            where_clause,
            group_by,
            having,
            order_by,
            limit,
        }))
    }

    // ── SELECT list ──────────────────────────────────────────────────────────

    fn parse_select_list(&mut self) -> Result<Vec<SelectItem>, String> {
        let mut items = Vec::new();
        loop {
            if self.peek() == &Token::Star {
                // Bare `*`
                self.advance();
                items.push(SelectItem::Wildcard);
            } else {
                let expr = self.parse_expr()?;

                // `table.*` is emitted by parse_primary as Column { table: Some(t), column: "*" }.
                // Convert it to QualifiedWildcard here in the SELECT-list context.
                let item = match expr {
                    Expression::Column { table: Some(t), column } if column == "*" => {
                        SelectItem::QualifiedWildcard(t)
                    }
                    other => {
                        let alias = if self.peek() == &Token::As {
                            self.advance();
                            match self.advance() {
                                Token::Identifier(name) => Some(name),
                                tok => {
                                    return Err(format!(
                                        "expected alias name after AS, got {:?}",
                                        tok
                                    ))
                                }
                            }
                        } else if let Token::Identifier(_) = self.peek() {
                            // Implicit alias: `expr name`
                            match self.advance() {
                                Token::Identifier(name) => Some(name),
                                _ => unreachable!(),
                            }
                        } else {
                            None
                        };
                        SelectItem::Expression(other, alias)
                    }
                };
                items.push(item);
            }

            if self.peek() == &Token::Comma {
                self.advance();
            } else {
                break;
            }
        }
        Ok(items)
    }

    // ── Table reference ──────────────────────────────────────────────────────

    fn parse_table_ref(&mut self) -> Result<TableRef, String> {
        let table_name = match self.advance() {
            Token::Identifier(name) => name,
            tok => return Err(format!("expected table name, got {:?}", tok)),
        };

        let alias = if self.peek() == &Token::As {
            self.advance();
            match self.advance() {
                Token::Identifier(name) => Some(name),
                tok => return Err(format!("expected alias after AS, got {:?}", tok)),
            }
        } else if let Token::Identifier(_) = self.peek() {
            match self.advance() {
                Token::Identifier(name) => Some(name),
                _ => unreachable!(),
            }
        } else {
            None
        };

        Ok(TableRef { table_name, alias })
    }

    // ── JOIN clause ──────────────────────────────────────────────────────────

    fn parse_join(&mut self) -> Result<Join, String> {
        let join_type = match self.peek().clone() {
            Token::Join => {
                self.advance();
                JoinType::Inner
            }
            Token::Inner => {
                self.advance();
                self.expect(&Token::Join)?;
                JoinType::Inner
            }
            Token::Left => {
                self.advance();
                self.expect(&Token::Join)?;
                JoinType::Left
            }
            Token::Right => {
                self.advance();
                self.expect(&Token::Join)?;
                JoinType::Right
            }
            Token::Cross => {
                self.advance();
                self.expect(&Token::Join)?;
                JoinType::Cross
            }
            tok => return Err(format!("expected JOIN keyword, got {:?}", tok)),
        };

        let table = self.parse_table_ref()?;
        self.expect(&Token::On)?;
        let condition = self.parse_expr()?;

        Ok(Join { join_type, table, condition })
    }

    // ── ORDER BY list ────────────────────────────────────────────────────────

    fn parse_order_by_list(&mut self) -> Result<Vec<OrderByItem>, String> {
        let mut items = Vec::new();
        loop {
            let expression = self.parse_expr()?;
            let ascending = match self.peek() {
                Token::Asc => {
                    self.advance();
                    true
                }
                Token::Desc => {
                    self.advance();
                    false
                }
                _ => true,
            };
            items.push(OrderByItem { expression, ascending });

            if self.peek() == &Token::Comma {
                self.advance();
            } else {
                break;
            }
        }
        Ok(items)
    }

    // ── Comma-separated expression list ──────────────────────────────────────

    fn parse_expr_list(&mut self) -> Result<Vec<Expression>, String> {
        let mut exprs = Vec::new();
        loop {
            exprs.push(self.parse_expr()?);
            if self.peek() == &Token::Comma {
                self.advance();
            } else {
                break;
            }
        }
        Ok(exprs)
    }

    // ── Expression parsing (recursive descent by precedence) ─────────────────
    //
    // Precedence levels (lowest → highest):
    //   OR
    //   AND
    //   NOT  (prefix, right-associative)
    //   comparison  (=  <>  <  >  <=  >=)   — non-chaining
    //   additive    (+  -)
    //   multiplicative  (*  /)
    //   unary minus  (-)                     — right-associative
    //   primary      (literal, column, function, parenthesised expr)

    pub fn parse_expr(&mut self) -> Result<Expression, String> {
        self.parse_or()
    }

    fn parse_or(&mut self) -> Result<Expression, String> {
        let mut left = self.parse_and()?;
        while self.peek() == &Token::Or {
            self.advance();
            let right = self.parse_and()?;
            left = Expression::BinaryOp {
                left: Box::new(left),
                op: BinaryOperator::Or,
                right: Box::new(right),
            };
        }
        Ok(left)
    }

    fn parse_and(&mut self) -> Result<Expression, String> {
        let mut left = self.parse_not()?;
        while self.peek() == &Token::And {
            self.advance();
            let right = self.parse_not()?;
            left = Expression::BinaryOp {
                left: Box::new(left),
                op: BinaryOperator::And,
                right: Box::new(right),
            };
        }
        Ok(left)
    }

    fn parse_not(&mut self) -> Result<Expression, String> {
        if self.peek() == &Token::Not {
            self.advance();
            let expr = self.parse_not()?; // right-associative: NOT NOT a => Not(Not(a))
            Ok(Expression::UnaryOp {
                op: UnaryOperator::Not,
                expr: Box::new(expr),
            })
        } else {
            self.parse_comparison()
        }
    }

    fn parse_comparison(&mut self) -> Result<Expression, String> {
        let left = self.parse_additive()?;
        let op = match self.peek() {
            Token::Eq  => BinaryOperator::Eq,
            Token::Neq => BinaryOperator::Neq,
            Token::Lt  => BinaryOperator::Lt,
            Token::Gt  => BinaryOperator::Gt,
            Token::Lte => BinaryOperator::Lte,
            Token::Gte => BinaryOperator::Gte,
            _ => return Ok(left),
        };
        self.advance();
        let right = self.parse_additive()?;
        Ok(Expression::BinaryOp {
            left: Box::new(left),
            op,
            right: Box::new(right),
        })
    }

    fn parse_additive(&mut self) -> Result<Expression, String> {
        let mut left = self.parse_multiplicative()?;
        loop {
            let op = match self.peek() {
                Token::Plus  => BinaryOperator::Add,
                Token::Minus => BinaryOperator::Sub,
                _ => break,
            };
            self.advance();
            let right = self.parse_multiplicative()?;
            left = Expression::BinaryOp {
                left: Box::new(left),
                op,
                right: Box::new(right),
            };
        }
        Ok(left)
    }

    fn parse_multiplicative(&mut self) -> Result<Expression, String> {
        let mut left = self.parse_unary()?;
        loop {
            let op = match self.peek() {
                Token::Star  => BinaryOperator::Mul,
                Token::Slash => BinaryOperator::Div,
                _ => break,
            };
            self.advance();
            let right = self.parse_unary()?;
            left = Expression::BinaryOp {
                left: Box::new(left),
                op,
                right: Box::new(right),
            };
        }
        Ok(left)
    }

    fn parse_unary(&mut self) -> Result<Expression, String> {
        if self.peek() == &Token::Minus {
            self.advance();
            let expr = self.parse_unary()?; // right-associative
            Ok(Expression::UnaryOp {
                op: UnaryOperator::Neg,
                expr: Box::new(expr),
            })
        } else {
            self.parse_primary()
        }
    }

    fn parse_primary(&mut self) -> Result<Expression, String> {
        match self.peek().clone() {
            // Parenthesised expression
            Token::LParen => {
                self.advance();
                let expr = self.parse_expr()?;
                self.expect(&Token::RParen)?;
                Ok(expr)
            }

            // Literals
            Token::Integer(n) => {
                self.advance();
                Ok(Expression::Literal(Literal::Integer(n)))
            }
            Token::Float(f) => {
                self.advance();
                Ok(Expression::Literal(Literal::Float(f)))
            }
            Token::StringLit(s) => {
                self.advance();
                Ok(Expression::Literal(Literal::Str(s)))
            }
            Token::True => {
                self.advance();
                Ok(Expression::Literal(Literal::Boolean(true)))
            }
            Token::False => {
                self.advance();
                Ok(Expression::Literal(Literal::Boolean(false)))
            }
            Token::Null => {
                self.advance();
                Ok(Expression::Literal(Literal::Null))
            }

            // Identifier: column, qualified column (t.col), table wildcard (t.*), or function call
            Token::Identifier(name) => {
                self.advance();

                // Function call: name(...)
                if self.peek() == &Token::LParen {
                    self.advance(); // consume '('
                    let args = if self.peek() == &Token::RParen {
                        Vec::new()
                    } else if self.peek() == &Token::Star {
                        // COUNT(*) — consume '*', yield Wildcard arg
                        self.advance();
                        vec![Expression::Wildcard]
                    } else {
                        self.parse_expr_list()?
                    };
                    self.expect(&Token::RParen)?;
                    return Ok(Expression::Function { name, args });
                }

                // Qualified: table.column or table.*
                if self.peek() == &Token::Dot {
                    self.advance(); // consume '.'
                    return match self.advance() {
                        Token::Identifier(col) => Ok(Expression::Column {
                            table: Some(name),
                            column: col,
                        }),
                        Token::Star => {
                            // table.* — represented as Column { table, column: "*" }
                            // so parse_select_list can convert it to QualifiedWildcard
                            Ok(Expression::Column {
                                table: Some(name),
                                column: "*".to_string(),
                            })
                        }
                        tok => Err(format!("expected column name after '.', got {:?}", tok)),
                    };
                }

                Ok(Expression::Column { table: None, column: name })
            }

            tok => Err(format!("unexpected token in expression: {:?}", tok)),
        }
    }
}

// ── Tests ────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use crate::parser::lexer::Lexer;

    fn parse(sql: &str) -> Statement {
        let tokens = Lexer::new(sql).tokenize().expect("lex error");
        Parser::new(tokens).parse().expect("parse error")
    }

    // Helper: parse the WHERE expression from a query
    fn where_expr(sql: &str) -> Expression {
        match parse(sql) {
            Statement::Select(s) => s.where_clause.expect("no WHERE clause"),
        }
    }

    // ── 1.1  Basic structural parsing ────────────────────────────────────────

    #[test]
    fn test_select_star() {
        let stmt = parse("SELECT * FROM orders");
        let Statement::Select(s) = stmt;
        assert!(matches!(s.columns[0], SelectItem::Wildcard));
        assert_eq!(s.from.table_name, "orders");
    }

    #[test]
    fn test_select_columns_with_alias() {
        let stmt = parse("SELECT id, name AS n FROM users");
        let Statement::Select(s) = stmt;
        assert_eq!(s.columns.len(), 2);
        if let SelectItem::Expression(_, alias) = &s.columns[1] {
            assert_eq!(alias.as_deref(), Some("n"));
        } else {
            panic!("expected Expression item");
        }
    }

    #[test]
    fn test_qualified_column() {
        let stmt = parse("SELECT c.name FROM customers c WHERE c.id = 1");
        let Statement::Select(s) = stmt;
        if let SelectItem::Expression(Expression::Column { table, column }, _) = &s.columns[0] {
            assert_eq!(table.as_deref(), Some("c"));
            assert_eq!(column, "name");
        } else {
            panic!("expected qualified column");
        }
    }

    #[test]
    fn test_inner_join() {
        let stmt = parse(
            "SELECT * FROM orders o INNER JOIN customers c ON o.customer_id = c.id",
        );
        let Statement::Select(s) = stmt;
        assert_eq!(s.joins.len(), 1);
        assert_eq!(s.joins[0].join_type, JoinType::Inner);
        assert_eq!(s.joins[0].table.table_name, "customers");
    }

    #[test]
    fn test_group_by_having() {
        let stmt = parse(
            "SELECT dept, COUNT(*) FROM emp GROUP BY dept HAVING COUNT(*) > 5",
        );
        let Statement::Select(s) = stmt;
        assert_eq!(s.group_by.len(), 1);
        assert!(s.having.is_some());
    }

    #[test]
    fn test_order_by_limit() {
        let stmt = parse("SELECT id FROM t ORDER BY id DESC LIMIT 10");
        let Statement::Select(s) = stmt;
        assert_eq!(s.order_by.len(), 1);
        assert!(!s.order_by[0].ascending);
        assert_eq!(s.limit, Some(10));
    }

    #[test]
    fn test_function_call() {
        let stmt = parse("SELECT COUNT(*), AVG(price) FROM products");
        let Statement::Select(s) = stmt;
        if let SelectItem::Expression(Expression::Function { name, args }, _) = &s.columns[0] {
            assert_eq!(name, "COUNT");
            assert!(matches!(args[0], Expression::Wildcard));
        } else {
            panic!("expected function");
        }
    }

    // ── 1.2  Expression precedence ───────────────────────────────────────────

    /// `a = 1 OR b = 2 AND c = 3`  must parse as  `OR(a=1, AND(b=2, c=3))`
    #[test]
    fn test_and_binds_tighter_than_or() {
        let expr = where_expr("SELECT 1 FROM t WHERE a = 1 OR b = 2 AND c = 3");
        // Top-level must be OR
        match expr {
            Expression::BinaryOp { op: BinaryOperator::Or, left, right } => {
                // left  = a = 1
                assert!(matches!(*left, Expression::BinaryOp { op: BinaryOperator::Eq, .. }));
                // right = AND(b=2, c=3)
                assert!(matches!(*right, Expression::BinaryOp { op: BinaryOperator::And, .. }));
            }
            other => panic!("expected OR at top level, got {:?}", other),
        }
    }

    /// `a + b * c`  must parse as  `a + (b * c)`
    #[test]
    fn test_mul_binds_tighter_than_add() {
        let expr = where_expr("SELECT 1 FROM t WHERE a + b * c = 0");
        match expr {
            Expression::BinaryOp { op: BinaryOperator::Eq, left, .. } => match *left {
                Expression::BinaryOp { op: BinaryOperator::Add, right, .. } => {
                    assert!(matches!(*right, Expression::BinaryOp { op: BinaryOperator::Mul, .. }));
                }
                other => panic!("expected Add, got {:?}", other),
            },
            other => panic!("expected Eq at top, got {:?}", other),
        }
    }

    /// `(a + b) * c`  — parentheses override precedence
    #[test]
    fn test_parentheses_override_precedence() {
        let expr = where_expr("SELECT 1 FROM t WHERE (a + b) * c = 0");
        match expr {
            Expression::BinaryOp { op: BinaryOperator::Eq, left, .. } => match *left {
                Expression::BinaryOp { op: BinaryOperator::Mul, left: inner, .. } => {
                    assert!(matches!(*inner, Expression::BinaryOp { op: BinaryOperator::Add, .. }));
                }
                other => panic!("expected Mul, got {:?}", other),
            },
            other => panic!("expected Eq at top, got {:?}", other),
        }
    }

    /// `NOT a = 1 AND b = 2`  =>  `AND(NOT(a=1), b=2)` — NOT binds tighter than AND
    #[test]
    fn test_not_binds_tighter_than_and() {
        let expr = where_expr("SELECT 1 FROM t WHERE NOT a = 1 AND b = 2");
        match expr {
            Expression::BinaryOp { op: BinaryOperator::And, left, .. } => {
                assert!(matches!(*left, Expression::UnaryOp { op: UnaryOperator::Not, .. }));
            }
            other => panic!("expected AND at top, got {:?}", other),
        }
    }

    /// Unary minus: `-a + b` => `Add(Neg(a), b)`
    #[test]
    fn test_unary_minus_precedence() {
        let expr = where_expr("SELECT 1 FROM t WHERE -a + b = 0");
        match expr {
            Expression::BinaryOp { op: BinaryOperator::Eq, left, .. } => match *left {
                Expression::BinaryOp { op: BinaryOperator::Add, left: neg, .. } => {
                    assert!(matches!(*neg, Expression::UnaryOp { op: UnaryOperator::Neg, .. }));
                }
                other => panic!("expected Add, got {:?}", other),
            },
            other => panic!("expected Eq at top, got {:?}", other),
        }
    }
}
