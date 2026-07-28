#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sql::parser {

// ── Operators ────────────────────────────────────────────────────────────────

enum class BinaryOperator {
    Eq, Neq, Lt, Gt, Lte, Gte, // comparison
    And, Or,                   // logical
    Add, Sub, Mul, Div,        // arithmetic
};

enum class UnaryOperator { Not, Neg };

// ── Literal values ───────────────────────────────────────────────────────────

struct Literal {
    enum class Kind { Integer, Float, Str, Boolean, Null };

    Kind kind = Kind::Null;
    int64_t int_val = 0;
    double float_val = 0.0;
    std::string str_val;
    bool bool_val = false;

    static Literal integer(int64_t v) { Literal l; l.kind = Kind::Integer; l.int_val = v; return l; }
    static Literal floating(double v) { Literal l; l.kind = Kind::Float; l.float_val = v; return l; }
    static Literal str(std::string v) { Literal l; l.kind = Kind::Str; l.str_val = std::move(v); return l; }
    static Literal boolean(bool v) { Literal l; l.kind = Kind::Boolean; l.bool_val = v; return l; }
    static Literal null() { return Literal{}; }
};

// ── Expressions (recursive — the core of everything) ────────────────────────
//
// Mirrors the Rust `Expression` enum. Recursive children are held behind
// unique_ptr; Expression provides a deep-copy copy constructor so that
// `.clone()`-style call sites in the Rust original translate directly to
// C++ copy construction/assignment (Rust's `derive(Clone)` on a type holding
// `Box<Expression>` performs the same deep clone).

class Expression {
public:
    enum class Kind { Column, Literal, BinaryOp, UnaryOp, Function, Wildcard };

    Kind kind = Kind::Wildcard;

    // Column { table, column }
    std::optional<std::string> table;
    std::string column;

    // Literal(Literal)
    sql::parser::Literal literal;

    // BinaryOp { left, op, right }
    std::unique_ptr<Expression> left;
    BinaryOperator binary_op = BinaryOperator::Eq;
    std::unique_ptr<Expression> right;

    // UnaryOp { op, expr }
    UnaryOperator unary_op = UnaryOperator::Not;
    std::unique_ptr<Expression> operand;

    // Function { name, args }
    std::string func_name;
    std::vector<Expression> args;

    Expression() = default;
    Expression(const Expression& other) { *this = other; }
    Expression(Expression&&) noexcept = default;
    Expression& operator=(Expression&&) noexcept = default;

    Expression& operator=(const Expression& other) {
        if (this == &other) return *this;
        kind = other.kind;
        table = other.table;
        column = other.column;
        literal = other.literal;
        left = other.left ? std::make_unique<Expression>(*other.left) : nullptr;
        binary_op = other.binary_op;
        right = other.right ? std::make_unique<Expression>(*other.right) : nullptr;
        unary_op = other.unary_op;
        operand = other.operand ? std::make_unique<Expression>(*other.operand) : nullptr;
        func_name = other.func_name;
        args = other.args;
        return *this;
    }

    static Expression make_column(std::optional<std::string> table, std::string column) {
        Expression e;
        e.kind = Kind::Column;
        e.table = std::move(table);
        e.column = std::move(column);
        return e;
    }

    static Expression make_literal(sql::parser::Literal l) {
        Expression e;
        e.kind = Kind::Literal;
        e.literal = std::move(l);
        return e;
    }

    static Expression make_binary_op(Expression l, BinaryOperator op, Expression r) {
        Expression e;
        e.kind = Kind::BinaryOp;
        e.left = std::make_unique<Expression>(std::move(l));
        e.binary_op = op;
        e.right = std::make_unique<Expression>(std::move(r));
        return e;
    }

    static Expression make_unary_op(UnaryOperator op, Expression operand) {
        Expression e;
        e.kind = Kind::UnaryOp;
        e.unary_op = op;
        e.operand = std::make_unique<Expression>(std::move(operand));
        return e;
    }

    static Expression make_function(std::string name, std::vector<Expression> args) {
        Expression e;
        e.kind = Kind::Function;
        e.func_name = std::move(name);
        e.args = std::move(args);
        return e;
    }

    static Expression make_wildcard() {
        Expression e;
        e.kind = Kind::Wildcard;
        return e;
    }
};

// ── SELECT list items ───────────────────────────────────────────────────────

struct SelectItem {
    enum class Kind { Expression, Wildcard, QualifiedWildcard };

    Kind kind = Kind::Wildcard;
    std::optional<sql::parser::Expression> expr; // Kind::Expression
    std::optional<std::string> alias;            // Kind::Expression
    std::string qualified_table;                 // Kind::QualifiedWildcard

    static SelectItem make_expression(sql::parser::Expression e, std::optional<std::string> alias) {
        SelectItem item;
        item.kind = Kind::Expression;
        item.expr = std::move(e);
        item.alias = std::move(alias);
        return item;
    }
    static SelectItem make_wildcard() {
        SelectItem item;
        item.kind = Kind::Wildcard;
        return item;
    }
    static SelectItem make_qualified_wildcard(std::string table) {
        SelectItem item;
        item.kind = Kind::QualifiedWildcard;
        item.qualified_table = std::move(table);
        return item;
    }
};

// ── Table reference (FROM / JOIN target) ────────────────────────────────────

struct TableRef {
    std::string table_name;
    std::optional<std::string> alias;
};

// ── JOIN ─────────────────────────────────────────────────────────────────────

enum class JoinType { Inner, Left, Right, Cross };

struct Join {
    JoinType join_type;
    TableRef table;
    Expression condition;
};

// ── ORDER BY ─────────────────────────────────────────────────────────────────

struct OrderByItem {
    Expression expression;
    bool ascending;
};

// ── SELECT query ─────────────────────────────────────────────────────────────

struct SelectStatement {
    std::vector<SelectItem> columns;
    TableRef from;
    std::vector<Join> joins;
    std::optional<Expression> where_clause;
    std::vector<Expression> group_by;
    std::optional<Expression> having;
    std::vector<OrderByItem> order_by;
    std::optional<size_t> limit;
};

// ── Top-level statement ──────────────────────────────────────────────────────
//
// Only SELECT is supported today; the Kind tag mirrors Rust's
// `enum Statement { Select(SelectStatement) }` so more statement kinds can be
// added later without disturbing call sites that already switch on `kind`.

struct Statement {
    enum class Kind { Select };
    Kind kind = Kind::Select;
    SelectStatement select;

    static Statement make_select(SelectStatement s) {
        Statement stmt;
        stmt.kind = Kind::Select;
        stmt.select = std::move(s);
        return stmt;
    }
};

} // namespace sql::parser
