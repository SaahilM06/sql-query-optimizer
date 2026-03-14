use std::collections::HashMap;

use crate::parser::ast::{Expression, SelectItem, SelectStatement, TableRef};
use crate::logical::logical_plan::{AggregateExpr, LogicalPlan};
use crate::logical::schema::Catalog;

/// Aggregate function names the planner recognises in the SELECT list.
const AGGREGATE_FUNCTIONS: &[&str] = &["SUM", "COUNT", "AVG", "MIN", "MAX"];

// ── Planner ──────────────────────────────────────────────────────────────────

pub struct LogicalPlanner<'a> {
    catalog: &'a Catalog,
    /// Maps every alias (or bare table name) used in the query → canonical table name.
    /// e.g. "c" → "customers", "o" → "orders"
    alias_map: HashMap<String, String>,
}

impl<'a> LogicalPlanner<'a> {
    pub fn new(catalog: &'a Catalog) -> Self {
        LogicalPlanner { catalog, alias_map: HashMap::new() }
    }

    /// Translate a parsed SELECT statement into a logical plan tree.
    pub fn plan(&mut self, stmt: SelectStatement) -> Result<LogicalPlan, String> {
        // ── 1. Build alias map so later resolution knows "c" = "customers" ──
        self.register_alias(&stmt.from);
        for join in &stmt.joins {
            self.register_alias(&join.table);
        }

        // ── 2. Scan for the FROM table ───────────────────────────────────────
        let mut plan = LogicalPlan::Scan {
            table_name: stmt.from.table_name.clone(),
            alias: stmt.from.alias.clone(),
        };

        // ── 3. Fold JOINs into a left-deep Join tree ─────────────────────────
        //
        //  Scan(a)
        //    └─ Join(b)       ← first join
        //         └─ Join(c)  ← second join
        //
        for join in stmt.joins {
            let right = LogicalPlan::Scan {
                table_name: join.table.table_name.clone(),
                alias: join.table.alias.clone(),
            };
            plan = LogicalPlan::Join {
                join_type: join.join_type,
                condition: join.condition,
                left: Box::new(plan),
                right: Box::new(right),
            };
        }

        // ── 4. WHERE → Filter ────────────────────────────────────────────────
        if let Some(predicate) = stmt.where_clause {
            plan = LogicalPlan::Filter {
                predicate,
                input: Box::new(plan),
            };
        }

        // ── 5. Split SELECT list into plain columns vs aggregate calls ────────
        let (plain_exprs, aggregates) = self.split_select_list(&stmt.columns)?;
        let has_aggregates = !aggregates.is_empty();
        let has_group_by   = !stmt.group_by.is_empty();

        // ── 6. GROUP BY + aggregates → Aggregate ─────────────────────────────
        if has_aggregates || has_group_by {
            plan = LogicalPlan::Aggregate {
                group_by: stmt.group_by,
                aggregates,
                input: Box::new(plan),
            };
        }

        // ── 7. HAVING → Filter (runs after aggregation) ──────────────────────
        if let Some(having) = stmt.having {
            plan = LogicalPlan::Filter {
                predicate: having,
                input: Box::new(plan),
            };
        }

        // ── 8. ORDER BY → Sort ───────────────────────────────────────────────
        if !stmt.order_by.is_empty() {
            plan = LogicalPlan::Sort {
                order_by: stmt.order_by,
                input: Box::new(plan),
            };
        }

        // ── 9. LIMIT ─────────────────────────────────────────────────────────
        if let Some(count) = stmt.limit {
            plan = LogicalPlan::Limit { count, input: Box::new(plan) };
        }

        // ── 10. Project for plain (non-aggregate) SELECT columns ─────────────
        //
        // Skip the Project node when:
        //   a) the query is SELECT * (wildcard) — nothing to project
        //   b) the query uses aggregates — Aggregate already defines the output
        //
        let is_wildcard_only = stmt.columns.iter()
            .all(|c| matches!(c, SelectItem::Wildcard | SelectItem::QualifiedWildcard(_)));

        if !is_wildcard_only && !has_aggregates {
            plan = LogicalPlan::Project {
                expressions: plain_exprs,
                input: Box::new(plan),
            };
        }

        Ok(plan)
    }

    // ── Helpers ──────────────────────────────────────────────────────────────

    fn register_alias(&mut self, table_ref: &TableRef) {
        let key = table_ref
            .alias
            .clone()
            .unwrap_or_else(|| table_ref.table_name.clone());
        self.alias_map.insert(key, table_ref.table_name.clone());
    }

    /// Walk the SELECT list and separate:
    /// - plain expressions  → go into a `Project` node
    /// - aggregate calls    → go into an `Aggregate` node
    fn split_select_list(
        &self,
        items: &[SelectItem],
    ) -> Result<(Vec<(Expression, Option<String>)>, Vec<AggregateExpr>), String> {
        let mut plain: Vec<(Expression, Option<String>)> = Vec::new();
        let mut aggs:  Vec<AggregateExpr>                = Vec::new();

        for item in items {
            match item {
                // Wildcards are pass-through — handled at a higher level
                SelectItem::Wildcard | SelectItem::QualifiedWildcard(_) => {}

                SelectItem::Expression(expr, alias) => {
                    if let Expression::Function { name, args } = expr {
                        if AGGREGATE_FUNCTIONS.contains(&name.to_uppercase().as_str()) {
                            let arg = args.first().cloned().unwrap_or(Expression::Wildcard);
                            aggs.push(AggregateExpr {
                                func: name.to_uppercase(),
                                arg,
                                alias: alias.clone(),
                            });
                            continue;
                        }
                    }
                    plain.push((expr.clone(), alias.clone()));
                }
            }
        }

        Ok((plain, aggs))
    }
}
