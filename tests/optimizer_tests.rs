// Integration tests for the logical optimizer.
//
// Each test:
//   1. Parses a SQL string into an AST
//   2. Runs the logical planner to get a raw LogicalPlan
//   3. Runs optimize() to get the rewritten plan
//   4. Asserts the tree has the expected shape
//
// Run with:   cargo test --test optimizer_tests
// See output: cargo test --test optimizer_tests -- --nocapture

use sql_query_optimizer::parser::ast::Statement;
use sql_query_optimizer::parser::lexer::Lexer;
use sql_query_optimizer::parser::parser::Parser;
use sql_query_optimizer::logical::logical_plan::LogicalPlan;
use sql_query_optimizer::logical::planner::LogicalPlanner;
use sql_query_optimizer::logical::schema::Catalog;
use sql_query_optimizer::logical::optimizer::optimize;

// ── pipeline helpers ──────────────────────────────────────────────────────────

fn plan(sql: &str) -> LogicalPlan {
    let tokens  = Lexer::new(sql).tokenize().expect("lex error");
    let stmt    = Parser::new(tokens).parse().expect("parse error");
    let catalog = Catalog::with_test_tables();
    let mut planner = LogicalPlanner::new(&catalog);
    match stmt {
        Statement::Select(s) => planner.plan(s).expect("plan error"),
    }
}

fn optimized(sql: &str) -> LogicalPlan {
    let raw     = plan(sql);
    let catalog = Catalog::with_test_tables();
    optimize(raw, &catalog)
}

// ── pretty-printer ────────────────────────────────────────────────────────────

fn print_plan(node: &LogicalPlan, depth: usize) {
    let pad = "  ".repeat(depth);
    match node {
        LogicalPlan::Scan { table_name, alias, projected_columns } => {
            let alias_str = alias.as_deref().unwrap_or("-");
            let cols = if projected_columns.is_empty() { "*".to_string() }
                       else { projected_columns.join(", ") };
            println!("{pad}Scan({table_name} AS {alias_str}) [cols: {cols}]");
        }
        LogicalPlan::Filter { predicate, input } => {
            println!("{pad}Filter({predicate:?})");
            print_plan(input, depth + 1);
        }
        LogicalPlan::Join { join_type, condition, left, right } => {
            println!("{pad}Join({join_type:?}, ON {condition:?})");
            print_plan(left,  depth + 1);
            print_plan(right, depth + 1);
        }
        LogicalPlan::Aggregate { group_by, aggregates, input } => {
            let aggs: Vec<_> = aggregates.iter()
                .map(|a| format!("{}({:?})", a.func, a.arg))
                .collect();
            println!("{pad}Aggregate(group={group_by:?}, aggs=[{}])", aggs.join(", "));
            print_plan(input, depth + 1);
        }
        LogicalPlan::Project { expressions, input } => {
            let cols: Vec<_> = expressions.iter()
                .map(|(e, alias)| match alias {
                    Some(a) => format!("{e:?} AS {a}"),
                    None    => format!("{e:?}"),
                })
                .collect();
            println!("{pad}Project([{}])", cols.join(", "));
            print_plan(input, depth + 1);
        }
        LogicalPlan::Sort { order_by, input } => {
            println!("{pad}Sort({order_by:?})");
            print_plan(input, depth + 1);
        }
        LogicalPlan::Limit { count, input } => {
            println!("{pad}Limit({count})");
            print_plan(input, depth + 1);
        }
    }
}

// ── tree-inspection helpers ───────────────────────────────────────────────────

fn node_kind(node: &LogicalPlan) -> &'static str {
    match node {
        LogicalPlan::Scan      { .. } => "Scan",
        LogicalPlan::Filter    { .. } => "Filter",
        LogicalPlan::Join      { .. } => "Join",
        LogicalPlan::Aggregate { .. } => "Aggregate",
        LogicalPlan::Project   { .. } => "Project",
        LogicalPlan::Sort      { .. } => "Sort",
        LogicalPlan::Limit     { .. } => "Limit",
    }
}

/// Count Filter nodes encountered while walking *down toward* the first Join.
/// Recurses through Project / Sort / Limit / Aggregate wrappers transparently.
fn filters_above_join(node: &LogicalPlan) -> usize {
    match node {
        LogicalPlan::Filter    { input, .. } => 1 + filters_above_join(input),
        LogicalPlan::Join      { .. }        => 0,
        // Transparent wrapper nodes — keep walking down.
        LogicalPlan::Project   { input, .. }
        | LogicalPlan::Aggregate { input, .. }
        | LogicalPlan::Sort    { input, .. }
        | LogicalPlan::Limit   { input, .. } => filters_above_join(input),
        LogicalPlan::Scan      { .. }        => 0,
    }
}

/// Count Filter nodes that live *inside* a Join's children (strictly below it).
/// First navigates down to the Join, then counts inside its left/right subtrees.
fn filters_below_join(node: &LogicalPlan) -> usize {
    match node {
        // Found the join — count filters inside each child independently.
        LogicalPlan::Join { left, right, .. } =>
            count_filters(left) + count_filters(right),
        // Transparent wrappers above the join — keep navigating down.
        LogicalPlan::Filter    { input, .. }
        | LogicalPlan::Project   { input, .. }
        | LogicalPlan::Aggregate { input, .. }
        | LogicalPlan::Sort    { input, .. }
        | LogicalPlan::Limit   { input, .. } => filters_below_join(input),
        LogicalPlan::Scan { .. } => 0,
    }
}

/// Count ALL Filter nodes anywhere in a subtree.
fn count_filters(node: &LogicalPlan) -> usize {
    match node {
        LogicalPlan::Filter { input, .. } => 1 + count_filters(input),
        LogicalPlan::Join   { left, right, .. } =>
            count_filters(left) + count_filters(right),
        LogicalPlan::Project   { input, .. }
        | LogicalPlan::Aggregate { input, .. }
        | LogicalPlan::Sort    { input, .. }
        | LogicalPlan::Limit   { input, .. } => count_filters(input),
        LogicalPlan::Scan { .. } => 0,
    }
}

/// True if `kind` appears anywhere in the plan tree.
fn contains_node(node: &LogicalPlan, kind: &str) -> bool {
    if node_kind(node) == kind { return true; }
    match node {
        LogicalPlan::Join { left, right, .. } =>
            contains_node(left, kind) || contains_node(right, kind),
        LogicalPlan::Filter    { input, .. }
        | LogicalPlan::Project   { input, .. }
        | LogicalPlan::Aggregate { input, .. }
        | LogicalPlan::Sort    { input, .. }
        | LogicalPlan::Limit   { input, .. } => contains_node(input, kind),
        LogicalPlan::Scan { .. } => false,
    }
}

/// Collect table names from Scan leaves in DFS order.
fn scan_names(node: &LogicalPlan, out: &mut Vec<String>) {
    match node {
        LogicalPlan::Scan { table_name, .. } => out.push(table_name.clone()),
        LogicalPlan::Join { left, right, .. } => {
            scan_names(left,  out);
            scan_names(right, out);
        }
        LogicalPlan::Filter    { input, .. }
        | LogicalPlan::Project   { input, .. }
        | LogicalPlan::Aggregate { input, .. }
        | LogicalPlan::Sort    { input, .. }
        | LogicalPlan::Limit   { input, .. } => scan_names(input, out),
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Rule 1 — Predicate Pushdown
// ═════════════════════════════════════════════════════════════════════════════

// Before: Filter(o.total > 100) → Join → [Scan(c), Scan(o)]
// After:  Join → [Scan(c), Filter(o.total > 100) → Scan(o)]
//
// The filter only references 'o' (orders) so it should be pushed to the
// right side of the join — no filters should remain above the join.
#[test]
fn predicate_pushed_below_join() {
    let sql = "
        SELECT c.name
        FROM customers c
        JOIN orders o ON c.id = o.customer_id
        WHERE o.total > 100
    ";

    let before = plan(sql);
    let after  = optimized(sql);

    println!("\n--- BEFORE ---");
    print_plan(&before, 0);
    println!("\n--- AFTER ---");
    print_plan(&after, 0);

    assert_eq!(filters_above_join(&after), 0,
        "predicate should have been pushed below the join — none should remain above it");
    assert_eq!(filters_below_join(&after), 1,
        "expected exactly one filter below the join (on the orders side)");
}

// WHERE has two conjuncts — one per table.
// Both should be pushed to their respective sides.
#[test]
fn predicate_both_sides_pushed() {
    let sql = "
        SELECT c.name
        FROM customers c
        JOIN orders o ON c.id = o.customer_id
        WHERE c.country = 'US' AND o.total > 500
    ";

    let before = plan(sql);
    let after  = optimized(sql);

    println!("\n--- BEFORE ---");
    print_plan(&before, 0);
    println!("\n--- AFTER ---");
    print_plan(&after, 0);

    assert_eq!(filters_above_join(&after), 0,
        "both predicates should be pushed below the join");
    assert_eq!(filters_below_join(&after), 2,
        "expected one filter on each side of the join");
}

// A predicate that references columns from BOTH tables cannot be pushed
// to either side — it must stay at or above the join.
#[test]
fn cross_table_predicate_stays_on_join() {
    let sql = "
        SELECT c.name
        FROM customers c
        JOIN orders o ON c.id = o.customer_id
        WHERE c.id = o.customer_id
    ";

    let after = optimized(sql);

    println!("\n--- AFTER ---");
    print_plan(&after, 0);

    // Nothing should land below the join — the predicate spans both sides.
    assert_eq!(filters_below_join(&after), 0,
        "cross-table predicate cannot be pushed to either side");
}

// ═════════════════════════════════════════════════════════════════════════════
// Rule 2 — Projection Pushdown
// ═════════════════════════════════════════════════════════════════════════════

// Only 'total' is needed.  The Scan should record only that column,
// not all four columns in the orders table.
#[test]
fn projection_pushdown_trims_scan_columns() {
    let sql = "
        SELECT o.total
        FROM orders o
        WHERE o.total > 100
    ";

    let after = optimized(sql);

    println!("\n--- AFTER ---");
    print_plan(&after, 0);

    fn find_scan_cols(node: &LogicalPlan) -> Option<&Vec<String>> {
        match node {
            LogicalPlan::Scan { projected_columns, .. } => Some(projected_columns),
            LogicalPlan::Filter  { input, .. }
            | LogicalPlan::Project { input, .. }
            | LogicalPlan::Sort  { input, .. }
            | LogicalPlan::Limit { input, .. } => find_scan_cols(input),
            _ => None,
        }
    }

    let cols = find_scan_cols(&after).expect("expected a Scan node");
    println!("Projected columns: {cols:?}");

    assert!(!cols.is_empty(),
        "projection pushdown should restrict the scan to needed columns");
    assert!(cols.contains(&"total".to_string()),
        "scan must include 'total'");
}

// SELECT * should leave projected_columns empty (= all columns).
#[test]
fn projection_pushdown_wildcard_keeps_all_columns() {
    let sql = "SELECT * FROM orders";

    let after = optimized(sql);

    println!("\n--- AFTER ---");
    print_plan(&after, 0);

    match &after {
        LogicalPlan::Scan { projected_columns, .. } => {
            assert!(projected_columns.is_empty(),
                "SELECT * should leave projected_columns empty (read all columns)");
        }
        other => panic!("expected Scan at root, got {}", node_kind(other)),
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Rule 3 — Join Reordering
// ═════════════════════════════════════════════════════════════════════════════

// Catalog row counts:
//   customers  →  10,000
//   orders     → 500,000
//   products   →   2,000
//
// Naive order (from SQL): customers → orders → products
// Optimal order:          products (2k) ⋈ customers (10k) first, then ⋈ orders
//
// After reordering, the DFS scan order should differ from the original.
#[test]
fn join_reorder_three_tables() {
    let sql = "
        SELECT c.name, o.total, p.name
        FROM customers c
        JOIN orders   o ON c.id  = o.customer_id
        JOIN products p ON o.id  = p.id
    ";

    let before = plan(sql);
    let after  = optimized(sql);

    println!("\n--- BEFORE ---");
    print_plan(&before, 0);
    println!("\n--- AFTER ---");
    print_plan(&after, 0);

    // The plan must still contain a Join after reordering.
    assert!(contains_node(&after, "Join"),
        "optimized plan should still contain a Join node");

    // The optimizer changed:
    //   BEFORE: (customers ⋈ orders) ⋈ products   — left child of top join is a Join
    //   AFTER:  customers ⋈ (orders ⋈ products)   — right child of top join is a Join
    //
    // This is the correct reordering because orders⋈products (100M est. rows)
    // is cheaper to produce first than customers⋈orders (500M est. rows).
    //
    // We detect the structural change by checking which child of the top Join
    // is itself a Join node.
    fn top_join_children(node: &LogicalPlan) -> Option<(&LogicalPlan, &LogicalPlan)> {
        match node {
            LogicalPlan::Join { left, right, .. } => Some((left, right)),
            LogicalPlan::Project { input, .. }
            | LogicalPlan::Sort   { input, .. }
            | LogicalPlan::Limit  { input, .. } => top_join_children(input),
            _ => None,
        }
    }

    let (before_l, before_r) = top_join_children(&before).expect("no Join in before plan");
    let (after_l,  after_r)  = top_join_children(&after) .expect("no Join in after plan");

    let before_nested_left  = matches!(before_l, LogicalPlan::Join { .. });
    let after_nested_right  = matches!(after_r,  LogicalPlan::Join { .. });

    println!("BEFORE: left-child is Join = {before_nested_left}");
    println!("AFTER:  right-child is Join = {after_nested_right}");

    // Before reordering: left-deep tree (left child is the inner join).
    assert!(before_nested_left,
        "original plan should be left-deep: left child of top join is a Join");

    // After reordering: right child should be the inner join (orders ⋈ products first).
    assert!(after_nested_right,
        "after reordering, orders ⋈ products should be the right (inner) join");
}

// Two-table join: nothing to reorder, but the plan must remain valid.
#[test]
fn join_reorder_two_tables_unchanged() {
    let sql = "
        SELECT c.name, o.total
        FROM customers c
        JOIN orders o ON c.id = o.customer_id
    ";

    let after = optimized(sql);

    println!("\n--- AFTER ---");
    print_plan(&after, 0);

    assert!(contains_node(&after, "Join"),
        "two-table join must still contain a Join node after optimization");
}

// ═════════════════════════════════════════════════════════════════════════════
// End-to-end: all three passes together
// ═════════════════════════════════════════════════════════════════════════════

// Aggregation + join + WHERE + GROUP BY.
// The WHERE filter should be pushed below the join.
// The top-level node should be Aggregate (planner doesn't add Project when
// aggregates are present).
#[test]
fn full_pipeline_aggregate_query() {
    let sql = "
        SELECT c.name, SUM(o.total)
        FROM customers c
        JOIN orders o ON c.id = o.customer_id
        WHERE o.total > 100
        GROUP BY c.name
    ";

    let before = plan(sql);
    let after  = optimized(sql);

    println!("\n=== BEFORE ===");
    print_plan(&before, 0);
    println!("\n=== AFTER ===");
    print_plan(&after, 0);

    // Planner skips Project when there are aggregates — top node is Aggregate.
    assert_eq!(node_kind(&after), "Aggregate",
        "top node should be Aggregate for a GROUP BY query");

    // WHERE filter on o.total should have been pushed below the join.
    assert_eq!(filters_above_join(&after), 0,
        "WHERE predicate should be pushed below the join");
    assert_eq!(filters_below_join(&after), 1,
        "expected one filter below the join (on the orders side)");
}

// ORDER BY + LIMIT + WHERE — correct wrapper order and filter pushdown.
#[test]
fn full_pipeline_limit_orderby() {
    let sql = "
        SELECT c.name
        FROM customers c
        JOIN orders o ON c.id = o.customer_id
        WHERE o.total > 200
        ORDER BY c.name
        LIMIT 10
    ";

    let before = plan(sql);
    let after  = optimized(sql);

    println!("\n=== BEFORE ===");
    print_plan(&before, 0);
    println!("\n=== AFTER ===");
    print_plan(&after, 0);

    // Plan structure:  Project → Limit → Sort → Join → ...
    // The top-level node is Project (wraps the column list).
    assert_eq!(node_kind(&after), "Project", "top node should be Project");

    // Limit and Sort must both be present somewhere in the tree.
    assert!(contains_node(&after, "Limit"), "plan must contain a Limit node");
    assert!(contains_node(&after, "Sort"),  "plan must contain a Sort node");

    // WHERE predicate should be pushed below the join.
    assert_eq!(filters_above_join(&after), 0,
        "WHERE predicate should be pushed below the join");
    assert_eq!(filters_below_join(&after), 1,
        "expected one filter below the join (on the orders side)");
}
