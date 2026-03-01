
#[derive(Debug, Clone)]
pub enum Statement {
    Select(SelectStatement),
}

// ── SELECT query ────────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct SelectStatement {
    pub columns: Vec<SelectItem>,
    pub from: TableRef,
    pub joins: Vec<Join>,
    pub where_clause: Option<Expression>,
    pub order_by: Vec<OrderByItem>,
    pub limit: Option<usize>,
}

// ── SELECT list items ───────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub enum SelectItem {
    Expression(Expression, Option<String>), // expr + optional alias
    Wildcard,                               // *
}

// ── Table reference (FROM / JOIN target) ────────────────────────────────────

#[derive(Debug, Clone)]
pub struct TableRef {
    pub table_name: String,
    pub alias: Option<String>,
}

// ── Expressions (recursive — the core of everything) ────────────────────────

#[derive(Debug, Clone)]
pub enum Expression {
    Column {
        table: Option<String>,
        column: String,
    },
    Literal(Literal),
    BinaryOp {
        left: Box<Expression>,
        op: BinaryOperator,
        right: Box<Expression>,
    },
    UnaryOp {
        op: UnaryOperator,
        expr: Box<Expression>,
    },
    Function {
        name: String,
        args: Vec<Expression>,
    },
}

// ── Literal values ──────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub enum Literal {
    Integer(i64),
    Float(f64),
    Str(String),
    Boolean(bool),
    Null,
}

// ── Operators ───────────────────────────────────────────────────────────────

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BinaryOperator {
    Eq,  // =
    Neq, // <> or !=
    Lt,  // <
    Gt,  // >
    Lte, // <=
    Gte, // >=
    And,
    Or,
    Add, // +
    Sub, // -
    Mul, // *
    Div, // /
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnaryOperator {
    Not,
    Neg,
}

// ── JOIN ────────────────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct Join {
    pub join_type: JoinType,
    pub table: TableRef,
    pub condition: Expression,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum JoinType {
    Inner,
    Left,
    Right,
    Cross,
}

// ── ORDER BY ────────────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct OrderByItem {
    pub expression: Expression,
    pub ascending: bool,
}
