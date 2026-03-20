use std::collections::HashSet;

use crate::logical::logical_plan::LogicalPlan;
use crate::logical::schema::Catalog;
use crate::parser::ast::{BinaryOperator, Expression, JoinType, Literal};

// ── Public entry point ────────────────────────────────────────────────────────

/// Run all three optimization passes in order.
pub fn optimize(plan: LogicalPlan, catalog: &Catalog) -> LogicalPlan {
    let plan = push_predicates(plan);
    let plan = push_projections(plan, &HashSet::new());
    reorder_joins(plan, catalog)
}

// ═════════════════════════════════════════════════════════════════════════════
// Rule 1 — Predicate Pushdown
//
// Move Filter nodes as close to their Scan as possible so joins operate on
// the smallest possible intermediate results.
// ═════════════════════════════════════════════════════════════════════════════

fn push_predicates(plan: LogicalPlan) -> LogicalPlan {
    match plan {
        // The interesting case: a Filter sitting directly above a Join.
        LogicalPlan::Filter { predicate, input } => {
            let input = push_predicates(*input);
            match input {
                LogicalPlan::Join { join_type, condition, left, right } => {
                    // Split "a AND b AND c" into [a, b, c]
                    let conjuncts     = split_and(predicate);
                    let left_aliases  = scan_aliases(&left);
                    let right_aliases = scan_aliases(&right);

                    let mut left_filters  = Vec::new();
                    let mut right_filters = Vec::new();
                    let mut remaining     = Vec::new();

                    for conj in conjuncts {
                        let tables   = referenced_tables(&conj);
                        let on_left  = tables.iter().any(|t| left_aliases.contains(t));
                        let on_right = tables.iter().any(|t| right_aliases.contains(t));
                        match (on_left, on_right) {
                            (true, false) => left_filters.push(conj),
                            (false, true) => right_filters.push(conj),
                            _             => remaining.push(conj), // spans both sides — stays on join
                        }
                    }

                    let new_left  = wrap_filters(*left,  left_filters);
                    let new_right = wrap_filters(*right, right_filters);
                    let join = LogicalPlan::Join {
                        join_type,
                        condition,
                        left:  Box::new(new_left),
                        right: Box::new(new_right),
                    };
                    wrap_filters(join, remaining)
                }
                // Filter over anything else — keep it, recurse already done.
                other => LogicalPlan::Filter { predicate, input: Box::new(other) },
            }
        }

        // Recurse into all other node types.
        LogicalPlan::Join { join_type, condition, left, right } => LogicalPlan::Join {
            join_type,
            condition,
            left:  Box::new(push_predicates(*left)),
            right: Box::new(push_predicates(*right)),
        },
        LogicalPlan::Aggregate { group_by, aggregates, input } => LogicalPlan::Aggregate {
            group_by,
            aggregates,
            input: Box::new(push_predicates(*input)),
        },
        LogicalPlan::Project { expressions, input } => LogicalPlan::Project {
            expressions,
            input: Box::new(push_predicates(*input)),
        },
        LogicalPlan::Sort { order_by, input } => LogicalPlan::Sort {
            order_by,
            input: Box::new(push_predicates(*input)),
        },
        LogicalPlan::Limit { count, input } => LogicalPlan::Limit {
            count,
            input: Box::new(push_predicates(*input)),
        },
        leaf => leaf, // Scan — nothing to do
    }
}

/// Stack `filters` as Filter nodes on top of `plan` (innermost first).
fn wrap_filters(mut plan: LogicalPlan, filters: Vec<Expression>) -> LogicalPlan {
    for p in filters {
        plan = LogicalPlan::Filter { predicate: p, input: Box::new(plan) };
    }
    plan
}

// ═════════════════════════════════════════════════════════════════════════════
// Rule 2 — Projection Pushdown
//
// Walk the tree top-down carrying the set of column names actually needed.
// At each Scan, record only those columns so the executor skips the rest.
// ═════════════════════════════════════════════════════════════════════════════

fn push_projections(plan: LogicalPlan, needed: &HashSet<String>) -> LogicalPlan {
    match plan {
        // Leaf: record which columns this scan should read.
        LogicalPlan::Scan { table_name, alias, .. } => {
            let projected_columns = if needed.is_empty() {
                vec![] // empty = all columns (no restriction from above)
            } else {
                needed.iter().cloned().collect()
            };
            LogicalPlan::Scan { table_name, alias, projected_columns }
        }

        // Project defines its own needed set.
        LogicalPlan::Project { expressions, input } => {
            let mut new_needed = columns_in_exprs(expressions.iter().map(|(e, _)| e));
            new_needed.extend(needed.iter().cloned());
            LogicalPlan::Project {
                expressions,
                input: Box::new(push_projections(*input, &new_needed)),
            }
        }

        // Filter needs the columns in its predicate.
        LogicalPlan::Filter { predicate, input } => {
            let mut new_needed = needed.clone();
            add_columns_from_expr(&predicate, &mut new_needed);
            LogicalPlan::Filter {
                predicate,
                input: Box::new(push_projections(*input, &new_needed)),
            }
        }

        // Join needs the columns in its condition on both sides.
        LogicalPlan::Join { join_type, condition, left, right } => {
            let mut new_needed = needed.clone();
            add_columns_from_expr(&condition, &mut new_needed);
            LogicalPlan::Join {
                join_type,
                condition,
                left:  Box::new(push_projections(*left,  &new_needed)),
                right: Box::new(push_projections(*right, &new_needed)),
            }
        }

        // Aggregate needs group keys + aggregate arguments.
        LogicalPlan::Aggregate { group_by, aggregates, input } => {
            let mut new_needed = needed.clone();
            for e in &group_by    { add_columns_from_expr(e,      &mut new_needed); }
            for a in &aggregates  { add_columns_from_expr(&a.arg, &mut new_needed); }
            LogicalPlan::Aggregate {
                group_by,
                aggregates,
                input: Box::new(push_projections(*input, &new_needed)),
            }
        }

        LogicalPlan::Sort { order_by, input } => {
            let mut new_needed = needed.clone();
            for item in &order_by { add_columns_from_expr(&item.expression, &mut new_needed); }
            LogicalPlan::Sort {
                order_by,
                input: Box::new(push_projections(*input, &new_needed)),
            }
        }

        LogicalPlan::Limit { count, input } => LogicalPlan::Limit {
            count,
            input: Box::new(push_projections(*input, needed)),
        },
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Rule 3 — Join Reordering
//
// For subtrees made entirely of INNER JOINs, flatten the join tree, then
// greedily rebuild it smallest-output-first using catalog row counts.
// Outer joins (LEFT/RIGHT) preserve their original order.
// ═════════════════════════════════════════════════════════════════════════════

fn reorder_joins(plan: LogicalPlan, catalog: &Catalog) -> LogicalPlan {
    match plan {
        LogicalPlan::Join { join_type, condition, left, right } => {
            // Recurse into children first so inner sub-trees are already optimal.
            let left  = Box::new(reorder_joins(*left,  catalog));
            let right = Box::new(reorder_joins(*right, catalog));
            let plan  = LogicalPlan::Join { join_type, condition, left, right };

            // Only reorder pure INNER-join subtrees — outer joins have order semantics.
            if !all_inner(&plan) {
                return plan;
            }

            let mut leaves: Vec<LogicalPlan> = Vec::new();
            let mut conds:  Vec<Expression>  = Vec::new();
            extract_join_tree(plan, &mut leaves, &mut conds);

            if leaves.len() >= 3 {
                greedy_join_order(leaves, conds, catalog)
            } else {
                rebuild_join_tree(leaves, conds)
            }
        }

        // Recurse into non-join nodes.
        LogicalPlan::Filter { predicate, input } => LogicalPlan::Filter {
            predicate,
            input: Box::new(reorder_joins(*input, catalog)),
        },
        LogicalPlan::Aggregate { group_by, aggregates, input } => LogicalPlan::Aggregate {
            group_by,
            aggregates,
            input: Box::new(reorder_joins(*input, catalog)),
        },
        LogicalPlan::Project { expressions, input } => LogicalPlan::Project {
            expressions,
            input: Box::new(reorder_joins(*input, catalog)),
        },
        LogicalPlan::Sort { order_by, input } => LogicalPlan::Sort {
            order_by,
            input: Box::new(reorder_joins(*input, catalog)),
        },
        LogicalPlan::Limit { count, input } => LogicalPlan::Limit {
            count,
            input: Box::new(reorder_joins(*input, catalog)),
        },
        leaf => leaf,
    }
}

/// True when every Join in this subtree is INNER.
fn all_inner(plan: &LogicalPlan) -> bool {
    match plan {
        LogicalPlan::Join { join_type, left, right, .. } => {
            *join_type == JoinType::Inner && all_inner(left) && all_inner(right)
        }
        _ => true,
    }
}

/// Flatten a left-deep join tree into (leaf plans, join conditions).
fn extract_join_tree(
    plan:   LogicalPlan,
    leaves: &mut Vec<LogicalPlan>,
    conds:  &mut Vec<Expression>,
) {
    match plan {
        LogicalPlan::Join { condition, left, right, .. } => {
            conds.push(condition);
            extract_join_tree(*left,  leaves, conds);
            extract_join_tree(*right, leaves, conds);
        }
        other => leaves.push(other),
    }
}

/// Rebuild a left-deep join tree from leaves + conditions (used for 2-table case).
fn rebuild_join_tree(mut leaves: Vec<LogicalPlan>, mut conds: Vec<Expression>) -> LogicalPlan {
    let mut tree = leaves.remove(0);
    for leaf in leaves {
        let cond = if conds.is_empty() {
            Expression::Literal(Literal::Boolean(true))
        } else {
            conds.remove(0)
        };
        tree = LogicalPlan::Join {
            join_type: JoinType::Inner,
            condition: cond,
            left:  Box::new(tree),
            right: Box::new(leaf),
        };
    }
    tree
}

/// Greedy join ordering: repeatedly pick the pair with the smallest estimated output.
fn greedy_join_order(
    mut leaves: Vec<LogicalPlan>,
    mut conds:  Vec<Expression>,
    catalog:    &Catalog,
) -> LogicalPlan {
    // Attach an estimated row count to each leaf.
    let mut nodes: Vec<(LogicalPlan, usize)> = leaves
        .drain(..)
        .map(|n| { let r = estimate_rows(&n, catalog); (n, r) })
        .collect();

    while nodes.len() > 1 {
        let (best_i, best_j, cond_idx) = best_pair(&nodes, &conds);

        let cond = match cond_idx {
            Some(ci) => conds.remove(ci),
            None     => Expression::Literal(Literal::Boolean(true)),
        };

        // Remove larger index first so the smaller index stays valid.
        let (right_plan, _) = nodes.remove(best_j);
        let (left_plan,  _) = nodes.remove(best_i);

        let new_rows = estimate_join_rows(
            estimate_rows(&left_plan,  catalog),
            estimate_rows(&right_plan, catalog),
        );
        let joined = LogicalPlan::Join {
            join_type: JoinType::Inner,
            condition: cond,
            left:  Box::new(left_plan),
            right: Box::new(right_plan),
        };
        nodes.push((joined, new_rows));
    }

    nodes.remove(0).0
}

/// Find the pair of nodes connected by a known condition with the smallest join output.
/// Returns (i, j, Option<condition_index>) where i < j.
fn best_pair(
    nodes: &[(LogicalPlan, usize)],
    conds: &[Expression],
) -> (usize, usize, Option<usize>) {
    let mut best_cost = usize::MAX;
    let mut best_i    = 0;
    let mut best_j    = 1;
    let mut best_ci   = None;

    for i in 0..nodes.len() {
        for j in (i + 1)..nodes.len() {
            let la = scan_aliases(&nodes[i].0);
            let ra = scan_aliases(&nodes[j].0);

            // Look for a condition that links these two relations.
            let ci = conds.iter().position(|c| {
                let tables   = referenced_tables(c);
                let on_left  = tables.iter().any(|t| la.contains(t));
                let on_right = tables.iter().any(|t| ra.contains(t));
                on_left && on_right
            });

            // Only consider pairs that share a join condition.
            if ci.is_some() {
                let cost = estimate_join_rows(nodes[i].1, nodes[j].1);
                if cost < best_cost {
                    best_cost = cost;
                    best_i    = i;
                    best_j    = j;
                    best_ci   = ci;
                }
            }
        }
    }

    (best_i, best_j, best_ci)
}

fn estimate_rows(plan: &LogicalPlan, catalog: &Catalog) -> usize {
    match plan {
        LogicalPlan::Scan { table_name, .. } =>
            catalog.get(table_name).map_or(1_000, |s| s.stats.row_count),
        LogicalPlan::Filter { input, .. } =>
            (estimate_rows(input, catalog) / 10).max(1),
        LogicalPlan::Join { left, right, .. } =>
            estimate_join_rows(estimate_rows(left, catalog), estimate_rows(right, catalog)),
        _ => 1_000,
    }
}

/// Equi-join heuristic: 10 % of the cartesian product, minimum 1.
fn estimate_join_rows(left: usize, right: usize) -> usize {
    ((left * right) / 10).max(1)
}

// ═════════════════════════════════════════════════════════════════════════════
// Shared expression helpers
// ═════════════════════════════════════════════════════════════════════════════

/// Flatten `a AND b AND c` → `[a, b, c]`.
fn split_and(expr: Expression) -> Vec<Expression> {
    match expr {
        Expression::BinaryOp { left, op: BinaryOperator::And, right } => {
            let mut v = split_and(*left);
            v.extend(split_and(*right));
            v
        }
        other => vec![other],
    }
}

/// Collect every table alias referenced by Column expressions inside `expr`.
fn referenced_tables(expr: &Expression) -> HashSet<String> {
    let mut out = HashSet::new();
    visit_tables(expr, &mut out);
    out
}

fn visit_tables(expr: &Expression, out: &mut HashSet<String>) {
    match expr {
        Expression::Column { table: Some(t), .. } => { out.insert(t.clone()); }
        Expression::Column { .. } => {}
        Expression::BinaryOp { left, right, .. } => {
            visit_tables(left,  out);
            visit_tables(right, out);
        }
        Expression::UnaryOp { expr, .. }          => visit_tables(expr, out),
        Expression::Function { args, .. }         => args.iter().for_each(|a| visit_tables(a, out)),
        Expression::Literal(_) | Expression::Wildcard => {}
    }
}

/// Add every column name (without table qualifier) from `expr` into `out`.
fn add_columns_from_expr(expr: &Expression, out: &mut HashSet<String>) {
    match expr {
        Expression::Column { column, .. } => { out.insert(column.clone()); }
        Expression::BinaryOp { left, right, .. } => {
            add_columns_from_expr(left,  out);
            add_columns_from_expr(right, out);
        }
        Expression::UnaryOp { expr, .. }  => add_columns_from_expr(expr, out),
        Expression::Function { args, .. } => args.iter().for_each(|a| add_columns_from_expr(a, out)),
        Expression::Literal(_) | Expression::Wildcard => {}
    }
}

fn columns_in_exprs<'a>(exprs: impl Iterator<Item = &'a Expression>) -> HashSet<String> {
    let mut out = HashSet::new();
    for e in exprs { add_columns_from_expr(e, &mut out); }
    out
}

/// Collect the alias (or bare table name) from every Scan reachable under `plan`.
fn scan_aliases(plan: &LogicalPlan) -> HashSet<String> {
    let mut out = HashSet::new();
    collect_aliases(plan, &mut out);
    out
}

fn collect_aliases(plan: &LogicalPlan, out: &mut HashSet<String>) {
    match plan {
        LogicalPlan::Scan { table_name, alias, .. } =>
            { out.insert(alias.clone().unwrap_or_else(|| table_name.clone())); }
        LogicalPlan::Join { left, right, .. } => {
            collect_aliases(left,  out);
            collect_aliases(right, out);
        }
        LogicalPlan::Filter    { input, .. }
        | LogicalPlan::Aggregate { input, .. }
        | LogicalPlan::Project   { input, .. }
        | LogicalPlan::Sort      { input, .. }
        | LogicalPlan::Limit     { input, .. } => collect_aliases(input, out),
    }
}
