#include "sql_printer.hpp"

#include <sstream>

namespace sql::parser {

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

std::string to_canonical_sql(const Statement& stmt) {
    std::ostringstream os;
    print_statement(stmt, os);
    return os.str();
}

} // namespace sql::parser
