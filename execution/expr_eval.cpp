#include "expr_eval.hpp"

#include <stdexcept>

#include "value_ops.hpp"

namespace sql::execution {

using namespace sql::parser;
using sql::storage::Row;

Literal evaluate(const Expression& expr, const Row& row, const RowSchema& schema) {
    switch (expr.kind) {
        case Expression::Kind::Literal:
            return expr.literal;

        case Expression::Kind::Column: {
            auto idx = schema.resolve(expr.table, expr.column);
            if (!idx.has_value()) {
                std::string qualified = (expr.table.has_value() ? *expr.table + "." : std::string()) + expr.column;
                throw std::runtime_error("execution: cannot resolve column '" + qualified + "'");
            }
            return row[*idx];
        }

        case Expression::Kind::UnaryOp: {
            Literal v = evaluate(*expr.operand, row, schema);
            if (expr.unary_op == UnaryOperator::Not) return Literal::boolean(!literal_truthy(v));
            if (v.kind == Literal::Kind::Integer) return Literal::integer(-v.int_val);
            return Literal::floating(-literal_as_double(v));
        }

        case Expression::Kind::BinaryOp: {
            Literal l = evaluate(*expr.left, row, schema);
            Literal r = evaluate(*expr.right, row, schema);
            switch (expr.binary_op) {
                case BinaryOperator::Eq: return Literal::boolean(literal_equal(l, r));
                case BinaryOperator::Neq: return Literal::boolean(!literal_equal(l, r));
                case BinaryOperator::Lt: return Literal::boolean(literal_less(l, r));
                case BinaryOperator::Gt: return Literal::boolean(literal_less(r, l));
                case BinaryOperator::Lte: return Literal::boolean(!literal_less(r, l));
                case BinaryOperator::Gte: return Literal::boolean(!literal_less(l, r));
                case BinaryOperator::And: return Literal::boolean(literal_truthy(l) && literal_truthy(r));
                case BinaryOperator::Or: return Literal::boolean(literal_truthy(l) || literal_truthy(r));
                case BinaryOperator::Add: return literal_add(l, r);
                case BinaryOperator::Sub: return literal_sub(l, r);
                case BinaryOperator::Mul: return literal_mul(l, r);
                case BinaryOperator::Div: return literal_div(l, r);
            }
            throw std::runtime_error("execution: unknown binary operator");
        }

        case Expression::Kind::Function: {
            // A HAVING/ORDER BY expression can re-mention an aggregate
            // directly (e.g. `HAVING SUM(o.total) > 100`) rather than
            // through an alias -- the logical planner doesn't rewrite these
            // to reference the aggregate's output column (see
            // LogicalPlanner::plan), so resolve it structurally against
            // whatever ancestor HashAggregate registered it instead.
            Expression arg = expr.args.empty() ? Expression::make_wildcard() : expr.args.front();
            auto idx = schema.resolve_aggregate(expr.func_name, arg);
            if (!idx.has_value()) {
                throw std::runtime_error("execution: cannot resolve aggregate reference '" + expr.func_name +
                                          "(...)' -- expected it to match an aggregate already computed by an "
                                          "ancestor HashAggregate");
            }
            return row[*idx];
        }

        case Expression::Kind::Wildcard:
            throw std::runtime_error("execution: a bare '*' cannot be evaluated as a row expression");
    }
    throw std::runtime_error("execution: unknown expression kind");
}

} // namespace sql::execution
