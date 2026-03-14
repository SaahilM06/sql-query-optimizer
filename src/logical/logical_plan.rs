use crate::parser::ast::{Expression, JoinType, OrderByItem};

// ── Aggregate helper ─────────────────────────────────────────────────────────

/// One aggregate call extracted from the SELECT list.
/// e.g. `SUM(o.total) AS total` → func="SUM", arg=Column(o.total), alias=Some("total")
#[derive(Debug, Clone)]
pub struct AggregateExpr {
    pub func: String,           // "SUM" | "COUNT" | "AVG" | "MIN" | "MAX"
    pub arg: Expression,        // the argument — Expression::Wildcard for COUNT(*)
    pub alias: Option<String>,
}

// ── Logical plan tree ────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub enum LogicalPlan {
    /// Read all rows from a table.
    Scan {
        table_name: String,
        alias: Option<String>,
    },

    /// Keep only rows where predicate is true.
    Filter {
        predicate: Expression,
        input: Box<LogicalPlan>,
    },

    /// Combine two inputs on a condition.
    Join {
        join_type: JoinType,
        condition: Expression,
        left: Box<LogicalPlan>,
        right: Box<LogicalPlan>,
    },

    /// Compute GROUP BY + aggregate functions.
    Aggregate {
        group_by: Vec<Expression>,
        aggregates: Vec<AggregateExpr>,
        input: Box<LogicalPlan>,
    },

    /// Emit only the listed expressions (the SELECT list).
    Project {
        expressions: Vec<(Expression, Option<String>)>, // (expr, alias)
        input: Box<LogicalPlan>,
    },

    /// Sort rows.
    Sort {
        order_by: Vec<OrderByItem>,
        input: Box<LogicalPlan>,
    },

    /// Return at most `count` rows.
    Limit {
        count: usize,
        input: Box<LogicalPlan>,
    },
}
