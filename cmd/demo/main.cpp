#include <array>
#include <iostream>

#include "logical/optimizer.hpp"
#include "logical/planner.hpp"
#include "logical/schema.hpp"
#include "optimizer/explain.hpp"
#include "parser/lexer.hpp"
#include "parser/parser.hpp"
#include "physical/physical_planner.hpp"
#include "statistics/statistics_loader.hpp"

using namespace sql::parser;

namespace {

const char* binary_op_name(BinaryOperator op) {
    switch (op) {
        case BinaryOperator::Eq: return "=";
        case BinaryOperator::Neq: return "<>";
        case BinaryOperator::Lt: return "<";
        case BinaryOperator::Gt: return ">";
        case BinaryOperator::Lte: return "<=";
        case BinaryOperator::Gte: return ">=";
        case BinaryOperator::And: return "AND";
        case BinaryOperator::Or: return "OR";
        case BinaryOperator::Add: return "+";
        case BinaryOperator::Sub: return "-";
        case BinaryOperator::Mul: return "*";
        case BinaryOperator::Div: return "/";
    }
    return "?";
}

void print_expr(const Expression& e, std::ostream& os) {
    switch (e.kind) {
        case Expression::Kind::Column:
            if (e.table.has_value()) os << *e.table << ".";
            os << e.column;
            break;
        case Expression::Kind::Literal:
            switch (e.literal.kind) {
                case Literal::Kind::Integer: os << e.literal.int_val; break;
                case Literal::Kind::Float: os << e.literal.float_val; break;
                case Literal::Kind::Str: os << "'" << e.literal.str_val << "'"; break;
                case Literal::Kind::Boolean: os << (e.literal.bool_val ? "TRUE" : "FALSE"); break;
                case Literal::Kind::Null: os << "NULL"; break;
            }
            break;
        case Expression::Kind::BinaryOp:
            os << "(";
            print_expr(*e.left, os);
            os << " " << binary_op_name(e.binary_op) << " ";
            print_expr(*e.right, os);
            os << ")";
            break;
        case Expression::Kind::UnaryOp:
            os << (e.unary_op == UnaryOperator::Not ? "NOT " : "-");
            print_expr(*e.operand, os);
            break;
        case Expression::Kind::Function:
            os << e.func_name << "(";
            for (size_t i = 0; i < e.args.size(); ++i) {
                if (i > 0) os << ", ";
                print_expr(e.args[i], os);
            }
            os << ")";
            break;
        case Expression::Kind::Wildcard:
            os << "*";
            break;
    }
}

void print_statement(const Statement& stmt, std::ostream& os) {
    const SelectStatement& s = stmt.select;
    os << "SELECT ";
    for (size_t i = 0; i < s.columns.size(); ++i) {
        if (i > 0) os << ", ";
        const SelectItem& item = s.columns[i];
        switch (item.kind) {
            case SelectItem::Kind::Wildcard: os << "*"; break;
            case SelectItem::Kind::QualifiedWildcard: os << item.qualified_table << ".*"; break;
            case SelectItem::Kind::Expression:
                print_expr(*item.expr, os);
                if (item.alias.has_value()) os << " AS " << *item.alias;
                break;
        }
    }
    os << " FROM " << s.from.table_name;
    if (s.from.alias.has_value()) os << " " << *s.from.alias;

    for (const auto& j : s.joins) {
        switch (j.join_type) {
            case JoinType::Inner: os << " INNER JOIN "; break;
            case JoinType::Left: os << " LEFT JOIN "; break;
            case JoinType::Right: os << " RIGHT JOIN "; break;
            case JoinType::Cross: os << " CROSS JOIN "; break;
        }
        os << j.table.table_name;
        if (j.table.alias.has_value()) os << " " << *j.table.alias;
        os << " ON ";
        print_expr(j.condition, os);
    }

    if (s.where_clause.has_value()) {
        os << " WHERE ";
        print_expr(*s.where_clause, os);
    }
    if (!s.group_by.empty()) {
        os << " GROUP BY ";
        for (size_t i = 0; i < s.group_by.size(); ++i) {
            if (i > 0) os << ", ";
            print_expr(s.group_by[i], os);
        }
    }
    if (s.having.has_value()) {
        os << " HAVING ";
        print_expr(*s.having, os);
    }
    if (!s.order_by.empty()) {
        os << " ORDER BY ";
        for (size_t i = 0; i < s.order_by.size(); ++i) {
            if (i > 0) os << ", ";
            print_expr(s.order_by[i].expression, os);
            os << (s.order_by[i].ascending ? " ASC" : " DESC");
        }
    }
    if (s.limit.has_value()) {
        os << " LIMIT " << *s.limit;
    }
}

} // namespace

int main() {
    const std::array<const char*, 4> queries = {
        "SELECT * FROM orders",
        "SELECT c.name, c.email FROM customers c WHERE c.country = 'US'",
        "SELECT o.id, SUM(o.total) FROM orders o "
        "INNER JOIN customers c ON o.customer_id = c.id "
        "WHERE c.active = TRUE "
        "GROUP BY o.id "
        "HAVING SUM(o.total) > 100 "
        "ORDER BY o.id DESC "
        "LIMIT 20",
        // precedence: OR(a=1, AND(b=2, c=3))
        "SELECT 1 FROM t WHERE a = 1 OR b = 2 AND c = 3",
    };

    for (const char* sql : queries) {
        std::cout << "SQL: " << sql << "\n";
        try {
            Lexer lexer(sql);
            std::vector<Token> tokens = lexer.tokenize();
            Parser parser(std::move(tokens));
            Statement ast = parser.parse();
            std::cout << "AST: ";
            print_statement(ast, std::cout);
            std::cout << "\n\n";
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << "\n\n";
        }
    }

    // ── Part 2 demo: cardinality/cost-annotated physical plan ────────────────
    //
    // Same worked example from the cardinality-estimation spec: an equi-join
    // with a WHERE predicate on each side, showing where every row estimate
    // came from.
    std::cout << "──────────────────────────────────────────────────────────\n";
    std::cout << "Annotated physical plan (Part 2: cardinality + cost)\n";
    std::cout << "──────────────────────────────────────────────────────────\n";

    const char* demo_sql =
        "SELECT c.name "
        "FROM customers c "
        "JOIN orders o ON c.id = o.customer_id "
        "WHERE c.country = 'US' AND o.total > 100";
    std::cout << "SQL: " << demo_sql << "\n\n";

    try {
        auto schema_catalog = sql::logical::Catalog::with_test_tables();
        auto stats_catalog = sql::statistics::load_catalog_from_directory(SQL_OPTIMIZER_STATS_DIR);

        Lexer lexer(demo_sql);
        Parser parser(lexer.tokenize());
        Statement stmt = parser.parse();

        sql::logical::LogicalPlanner logical_planner(schema_catalog);
        auto logical_plan = logical_planner.plan(std::move(stmt.select));
        auto optimized = sql::logical::optimize(std::move(logical_plan), schema_catalog);

        auto physical_plan = sql::physical::generate_physical_plan(optimized, schema_catalog, stats_catalog);

        std::cout << "Chosen top-level strategy: "
                   << (physical_plan.kind == sql::physical::PhysicalPlan::Kind::Project ? "Project" : "other") << "\n\n";
        sql::optimizer::explain_plan(physical_plan, std::cout);
    } catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << "\n";
    }

    // ── Part 3 demo: cost-based join order search ────────────────────────────
    //
    // The spec's own worked example: three tables joined in SQL as
    // customers -> orders -> products, with a selective filter on each end
    // (customers.country and products.category) but none on orders. The DP
    // search should discover that starting with orders <-> products (the
    // more selective pair once products is filtered) beats blindly
    // following the SQL FROM/JOIN order.
    std::cout << "\n──────────────────────────────────────────────────────────\n";
    std::cout << "Cost-based join order search (Part 3)\n";
    std::cout << "──────────────────────────────────────────────────────────\n";

    const char* join_search_sql =
        "SELECT c.name "
        "FROM customers c "
        "JOIN orders o ON c.id = o.customer_id "
        "JOIN products p ON o.product_id = p.id "
        "WHERE c.country = 'US' AND p.category = 'electronics'";
    std::cout << "SQL: " << join_search_sql << "\n";
    std::cout << "(SQL join order as written: customers -> orders -> products)\n\n";

    try {
        auto schema_catalog = sql::logical::Catalog::with_test_tables();
        auto stats_catalog = sql::statistics::load_catalog_from_directory(SQL_OPTIMIZER_STATS_DIR);

        Lexer lexer(join_search_sql);
        Parser parser(lexer.tokenize());
        Statement stmt = parser.parse();

        sql::logical::LogicalPlanner logical_planner(schema_catalog);
        auto logical_plan = logical_planner.plan(std::move(stmt.select));
        auto optimized = sql::logical::optimize(std::move(logical_plan), schema_catalog);

        auto physical_plan = sql::physical::generate_physical_plan(optimized, schema_catalog, stats_catalog);

        std::cout << "Chosen physical plan:\n\n";
        sql::optimizer::explain_plan(physical_plan, std::cout);
    } catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << "\n";
    }

    return 0;
}
