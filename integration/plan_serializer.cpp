#include "plan_serializer.hpp"

#include <stdexcept>

#include "../util/json.hpp"

namespace sql::integration {

using sql::logical::AggregateExpr;
using sql::parser::BinaryOperator;
using sql::parser::Expression;
using sql::parser::JoinType;
using sql::parser::Literal;
using sql::parser::OrderByItem;
using sql::parser::UnaryOperator;
using sql::physical::PhysicalPlan;
using sql::util::JsonValue;

namespace {

// ── Enum <-> string ──────────────────────────────────────────────────────────
//
// Strings rather than raw ints so the JSON stays human-debuggable and isn't
// silently corrupted if an enum is ever reordered.

std::string kind_to_string(Expression::Kind k) {
    switch (k) {
        case Expression::Kind::Column: return "Column";
        case Expression::Kind::Literal: return "Literal";
        case Expression::Kind::BinaryOp: return "BinaryOp";
        case Expression::Kind::UnaryOp: return "UnaryOp";
        case Expression::Kind::Function: return "Function";
        case Expression::Kind::Wildcard: return "Wildcard";
    }
    throw std::runtime_error("plan_serializer: unknown Expression::Kind");
}

Expression::Kind expr_kind_from_string(const std::string& s) {
    if (s == "Column") return Expression::Kind::Column;
    if (s == "Literal") return Expression::Kind::Literal;
    if (s == "BinaryOp") return Expression::Kind::BinaryOp;
    if (s == "UnaryOp") return Expression::Kind::UnaryOp;
    if (s == "Function") return Expression::Kind::Function;
    if (s == "Wildcard") return Expression::Kind::Wildcard;
    throw std::runtime_error("plan_serializer: unknown Expression::Kind '" + s + "'");
}

std::string kind_to_string(Literal::Kind k) {
    switch (k) {
        case Literal::Kind::Integer: return "Integer";
        case Literal::Kind::Float: return "Float";
        case Literal::Kind::Str: return "Str";
        case Literal::Kind::Boolean: return "Boolean";
        case Literal::Kind::Null: return "Null";
    }
    throw std::runtime_error("plan_serializer: unknown Literal::Kind");
}

Literal::Kind literal_kind_from_string(const std::string& s) {
    if (s == "Integer") return Literal::Kind::Integer;
    if (s == "Float") return Literal::Kind::Float;
    if (s == "Str") return Literal::Kind::Str;
    if (s == "Boolean") return Literal::Kind::Boolean;
    if (s == "Null") return Literal::Kind::Null;
    throw std::runtime_error("plan_serializer: unknown Literal::Kind '" + s + "'");
}

std::string op_to_string(BinaryOperator op) {
    switch (op) {
        case BinaryOperator::Eq: return "Eq";
        case BinaryOperator::Neq: return "Neq";
        case BinaryOperator::Lt: return "Lt";
        case BinaryOperator::Gt: return "Gt";
        case BinaryOperator::Lte: return "Lte";
        case BinaryOperator::Gte: return "Gte";
        case BinaryOperator::And: return "And";
        case BinaryOperator::Or: return "Or";
        case BinaryOperator::Add: return "Add";
        case BinaryOperator::Sub: return "Sub";
        case BinaryOperator::Mul: return "Mul";
        case BinaryOperator::Div: return "Div";
    }
    throw std::runtime_error("plan_serializer: unknown BinaryOperator");
}

BinaryOperator binary_op_from_string(const std::string& s) {
    if (s == "Eq") return BinaryOperator::Eq;
    if (s == "Neq") return BinaryOperator::Neq;
    if (s == "Lt") return BinaryOperator::Lt;
    if (s == "Gt") return BinaryOperator::Gt;
    if (s == "Lte") return BinaryOperator::Lte;
    if (s == "Gte") return BinaryOperator::Gte;
    if (s == "And") return BinaryOperator::And;
    if (s == "Or") return BinaryOperator::Or;
    if (s == "Add") return BinaryOperator::Add;
    if (s == "Sub") return BinaryOperator::Sub;
    if (s == "Mul") return BinaryOperator::Mul;
    if (s == "Div") return BinaryOperator::Div;
    throw std::runtime_error("plan_serializer: unknown BinaryOperator '" + s + "'");
}

std::string op_to_string(UnaryOperator op) {
    switch (op) {
        case UnaryOperator::Not: return "Not";
        case UnaryOperator::Neg: return "Neg";
    }
    throw std::runtime_error("plan_serializer: unknown UnaryOperator");
}

UnaryOperator unary_op_from_string(const std::string& s) {
    if (s == "Not") return UnaryOperator::Not;
    if (s == "Neg") return UnaryOperator::Neg;
    throw std::runtime_error("plan_serializer: unknown UnaryOperator '" + s + "'");
}

std::string join_type_to_string(JoinType jt) {
    switch (jt) {
        case JoinType::Inner: return "Inner";
        case JoinType::Left: return "Left";
        case JoinType::Right: return "Right";
        case JoinType::Cross: return "Cross";
    }
    throw std::runtime_error("plan_serializer: unknown JoinType");
}

JoinType join_type_from_string(const std::string& s) {
    if (s == "Inner") return JoinType::Inner;
    if (s == "Left") return JoinType::Left;
    if (s == "Right") return JoinType::Right;
    if (s == "Cross") return JoinType::Cross;
    throw std::runtime_error("plan_serializer: unknown JoinType '" + s + "'");
}

std::string plan_kind_to_string(PhysicalPlan::Kind k) {
    switch (k) {
        case PhysicalPlan::Kind::SeqScan: return "SeqScan";
        case PhysicalPlan::Kind::IndexScan: return "IndexScan";
        case PhysicalPlan::Kind::Filter: return "Filter";
        case PhysicalPlan::Kind::NestedLoopJoin: return "NestedLoopJoin";
        case PhysicalPlan::Kind::HashJoin: return "HashJoin";
        case PhysicalPlan::Kind::IndexNestedLoopJoin: return "IndexNestedLoopJoin";
        case PhysicalPlan::Kind::HashAggregate: return "HashAggregate";
        case PhysicalPlan::Kind::Project: return "Project";
        case PhysicalPlan::Kind::Sort: return "Sort";
        case PhysicalPlan::Kind::Limit: return "Limit";
        case PhysicalPlan::Kind::ExternalRows: return "ExternalRows";
    }
    throw std::runtime_error("plan_serializer: unknown PhysicalPlan::Kind");
}

PhysicalPlan::Kind plan_kind_from_string(const std::string& s) {
    if (s == "SeqScan") return PhysicalPlan::Kind::SeqScan;
    if (s == "IndexScan") return PhysicalPlan::Kind::IndexScan;
    if (s == "Filter") return PhysicalPlan::Kind::Filter;
    if (s == "NestedLoopJoin") return PhysicalPlan::Kind::NestedLoopJoin;
    if (s == "HashJoin") return PhysicalPlan::Kind::HashJoin;
    if (s == "IndexNestedLoopJoin") return PhysicalPlan::Kind::IndexNestedLoopJoin;
    if (s == "HashAggregate") return PhysicalPlan::Kind::HashAggregate;
    if (s == "Project") return PhysicalPlan::Kind::Project;
    if (s == "Sort") return PhysicalPlan::Kind::Sort;
    if (s == "Limit") return PhysicalPlan::Kind::Limit;
    if (s == "ExternalRows") return PhysicalPlan::Kind::ExternalRows;
    throw std::runtime_error("plan_serializer: unknown PhysicalPlan::Kind '" + s + "'");
}

// ── Expression <-> JSON ──────────────────────────────────────────────────────

JsonValue literal_to_json(const Literal& lit) {
    JsonValue j = JsonValue::make_object();
    j.object_val["kind"] = JsonValue::make_string(kind_to_string(lit.kind));
    j.object_val["int_val"] = JsonValue::make_number(static_cast<double>(lit.int_val));
    j.object_val["float_val"] = JsonValue::make_number(lit.float_val);
    j.object_val["str_val"] = JsonValue::make_string(lit.str_val);
    j.object_val["bool_val"] = JsonValue::make_bool(lit.bool_val);
    return j;
}

Literal literal_from_json(const JsonValue& j) {
    Literal lit;
    if (const auto* k = j.find("kind")) lit.kind = literal_kind_from_string(k->as_string());
    if (const auto* v = j.find("int_val")) lit.int_val = static_cast<int64_t>(v->as_number());
    if (const auto* v = j.find("float_val")) lit.float_val = v->as_number();
    if (const auto* v = j.find("str_val")) lit.str_val = v->as_string();
    if (const auto* v = j.find("bool_val")) lit.bool_val = v->as_bool();
    return lit;
}

JsonValue expr_to_json(const Expression& e) {
    JsonValue j = JsonValue::make_object();
    j.object_val["kind"] = JsonValue::make_string(kind_to_string(e.kind));
    j.object_val["table"] = e.table ? JsonValue::make_string(*e.table) : JsonValue::make_null();
    j.object_val["column"] = JsonValue::make_string(e.column);
    j.object_val["literal"] = literal_to_json(e.literal);
    j.object_val["left"] = e.left ? expr_to_json(*e.left) : JsonValue::make_null();
    j.object_val["binary_op"] = JsonValue::make_string(op_to_string(e.binary_op));
    j.object_val["right"] = e.right ? expr_to_json(*e.right) : JsonValue::make_null();
    j.object_val["unary_op"] = JsonValue::make_string(op_to_string(e.unary_op));
    j.object_val["operand"] = e.operand ? expr_to_json(*e.operand) : JsonValue::make_null();
    j.object_val["func_name"] = JsonValue::make_string(e.func_name);
    JsonValue args = JsonValue::make_array();
    for (const auto& a : e.args) args.array_val.push_back(expr_to_json(a));
    j.object_val["args"] = std::move(args);
    return j;
}

Expression expr_from_json(const JsonValue& j) {
    Expression e;
    if (const auto* k = j.find("kind")) e.kind = expr_kind_from_string(k->as_string());
    if (const auto* t = j.find("table"); t != nullptr && t->kind == JsonValue::Kind::String) e.table = t->str_val;
    if (const auto* c = j.find("column")) e.column = c->as_string();
    if (const auto* l = j.find("literal")) e.literal = literal_from_json(*l);
    if (const auto* l = j.find("left"); l != nullptr && l->kind == JsonValue::Kind::Object) {
        e.left = std::make_unique<Expression>(expr_from_json(*l));
    }
    if (const auto* b = j.find("binary_op")) e.binary_op = binary_op_from_string(b->as_string());
    if (const auto* r = j.find("right"); r != nullptr && r->kind == JsonValue::Kind::Object) {
        e.right = std::make_unique<Expression>(expr_from_json(*r));
    }
    if (const auto* u = j.find("unary_op")) e.unary_op = unary_op_from_string(u->as_string());
    if (const auto* o = j.find("operand"); o != nullptr && o->kind == JsonValue::Kind::Object) {
        e.operand = std::make_unique<Expression>(expr_from_json(*o));
    }
    if (const auto* f = j.find("func_name")) e.func_name = f->as_string();
    if (const auto* a = j.find("args"); a != nullptr && a->kind == JsonValue::Kind::Array) {
        for (const auto& item : a->array_val) e.args.push_back(expr_from_json(item));
    }
    return e;
}

// ── AggregateExpr / OrderByItem <-> JSON ─────────────────────────────────────

JsonValue aggregate_to_json(const AggregateExpr& agg) {
    JsonValue j = JsonValue::make_object();
    j.object_val["func"] = JsonValue::make_string(agg.func);
    j.object_val["arg"] = expr_to_json(agg.arg);
    j.object_val["alias"] = agg.alias ? JsonValue::make_string(*agg.alias) : JsonValue::make_null();
    return j;
}

AggregateExpr aggregate_from_json(const JsonValue& j) {
    AggregateExpr agg;
    if (const auto* f = j.find("func")) agg.func = f->as_string();
    if (const auto* a = j.find("arg")) agg.arg = expr_from_json(*a);
    if (const auto* al = j.find("alias"); al != nullptr && al->kind == JsonValue::Kind::String) agg.alias = al->str_val;
    return agg;
}

JsonValue order_by_to_json(const OrderByItem& item) {
    JsonValue j = JsonValue::make_object();
    j.object_val["expression"] = expr_to_json(item.expression);
    j.object_val["ascending"] = JsonValue::make_bool(item.ascending);
    return j;
}

OrderByItem order_by_from_json(const JsonValue& j) {
    OrderByItem item;
    if (const auto* e = j.find("expression")) item.expression = expr_from_json(*e);
    if (const auto* a = j.find("ascending")) item.ascending = a->as_bool();
    return item;
}

// ── Cost <-> JSON ─────────────────────────────────────────────────────────────

JsonValue cost_to_json(const sql::physical::cost::Cost& c) {
    JsonValue j = JsonValue::make_object();
    j.object_val["cpu"] = JsonValue::make_number(c.cpu);
    j.object_val["io"] = JsonValue::make_number(c.io);
    j.object_val["memory"] = JsonValue::make_number(c.memory);
    return j;
}

sql::physical::cost::Cost cost_from_json(const JsonValue& j) {
    sql::physical::cost::Cost c;
    if (const auto* v = j.find("cpu")) c.cpu = v->as_number();
    if (const auto* v = j.find("io")) c.io = v->as_number();
    if (const auto* v = j.find("memory")) c.memory = v->as_number();
    return c;
}

// ── PhysicalPlan <-> JSON ─────────────────────────────────────────────────────

JsonValue plan_to_json(const PhysicalPlan& p) {
    JsonValue j = JsonValue::make_object();
    j.object_val["kind"] = JsonValue::make_string(plan_kind_to_string(p.kind));
    j.object_val["estimated_cost"] = cost_to_json(p.estimated_cost);
    j.object_val["estimated_rows"] = JsonValue::make_number(static_cast<double>(p.estimated_rows));
    j.object_val["cardinality_reasoning"] = JsonValue::make_string(p.cardinality_reasoning);
    j.object_val["cardinality_confidence"] = JsonValue::make_number(p.cardinality_confidence);

    j.object_val["table_name"] = JsonValue::make_string(p.table_name);
    j.object_val["alias"] = p.alias ? JsonValue::make_string(*p.alias) : JsonValue::make_null();
    JsonValue proj_cols = JsonValue::make_array();
    for (const auto& c : p.projected_columns) proj_cols.array_val.push_back(JsonValue::make_string(c));
    j.object_val["projected_columns"] = std::move(proj_cols);
    j.object_val["index_column"] = JsonValue::make_string(p.index_column);
    j.object_val["index_probe_value"] = expr_to_json(p.index_probe_value);

    j.object_val["predicate"] = expr_to_json(p.predicate);
    j.object_val["input"] = p.input ? plan_to_json(*p.input) : JsonValue::make_null();

    j.object_val["join_type"] = JsonValue::make_string(join_type_to_string(p.join_type));
    j.object_val["condition"] = expr_to_json(p.condition);
    j.object_val["left"] = p.left ? plan_to_json(*p.left) : JsonValue::make_null();
    j.object_val["right"] = p.right ? plan_to_json(*p.right) : JsonValue::make_null();

    JsonValue group_by = JsonValue::make_array();
    for (const auto& g : p.group_by) group_by.array_val.push_back(expr_to_json(g));
    j.object_val["group_by"] = std::move(group_by);

    JsonValue aggregates = JsonValue::make_array();
    for (const auto& a : p.aggregates) aggregates.array_val.push_back(aggregate_to_json(a));
    j.object_val["aggregates"] = std::move(aggregates);

    JsonValue expressions = JsonValue::make_array();
    for (const auto& [expr, alias] : p.expressions) {
        JsonValue pair_json = JsonValue::make_object();
        pair_json.object_val["expr"] = expr_to_json(expr);
        pair_json.object_val["alias"] = alias ? JsonValue::make_string(*alias) : JsonValue::make_null();
        expressions.array_val.push_back(std::move(pair_json));
    }
    j.object_val["expressions"] = std::move(expressions);

    JsonValue order_by = JsonValue::make_array();
    for (const auto& o : p.order_by) order_by.array_val.push_back(order_by_to_json(o));
    j.object_val["order_by"] = std::move(order_by);

    j.object_val["count"] = JsonValue::make_number(static_cast<double>(p.count));

    return j;
}

PhysicalPlan plan_from_json(const JsonValue& j) {
    PhysicalPlan p;
    if (const auto* k = j.find("kind")) p.kind = plan_kind_from_string(k->as_string());
    if (const auto* c = j.find("estimated_cost")) p.estimated_cost = cost_from_json(*c);
    if (const auto* r = j.find("estimated_rows")) p.estimated_rows = static_cast<size_t>(r->as_number());
    if (const auto* r = j.find("cardinality_reasoning")) p.cardinality_reasoning = r->as_string();
    if (const auto* c = j.find("cardinality_confidence")) p.cardinality_confidence = c->as_number();

    if (const auto* t = j.find("table_name")) p.table_name = t->as_string();
    if (const auto* a = j.find("alias"); a != nullptr && a->kind == JsonValue::Kind::String) p.alias = a->str_val;
    if (const auto* pc = j.find("projected_columns"); pc != nullptr && pc->kind == JsonValue::Kind::Array) {
        for (const auto& item : pc->array_val) p.projected_columns.push_back(item.as_string());
    }
    if (const auto* ic = j.find("index_column")) p.index_column = ic->as_string();
    if (const auto* ipv = j.find("index_probe_value")) p.index_probe_value = expr_from_json(*ipv);

    if (const auto* pred = j.find("predicate")) p.predicate = expr_from_json(*pred);
    if (const auto* in = j.find("input"); in != nullptr && in->kind == JsonValue::Kind::Object) {
        p.input = std::make_unique<PhysicalPlan>(plan_from_json(*in));
    }

    if (const auto* jt = j.find("join_type")) p.join_type = join_type_from_string(jt->as_string());
    if (const auto* cond = j.find("condition")) p.condition = expr_from_json(*cond);
    if (const auto* l = j.find("left"); l != nullptr && l->kind == JsonValue::Kind::Object) {
        p.left = std::make_unique<PhysicalPlan>(plan_from_json(*l));
    }
    if (const auto* r = j.find("right"); r != nullptr && r->kind == JsonValue::Kind::Object) {
        p.right = std::make_unique<PhysicalPlan>(plan_from_json(*r));
    }

    if (const auto* gb = j.find("group_by"); gb != nullptr && gb->kind == JsonValue::Kind::Array) {
        for (const auto& item : gb->array_val) p.group_by.push_back(expr_from_json(item));
    }
    if (const auto* ag = j.find("aggregates"); ag != nullptr && ag->kind == JsonValue::Kind::Array) {
        for (const auto& item : ag->array_val) p.aggregates.push_back(aggregate_from_json(item));
    }
    if (const auto* ex = j.find("expressions"); ex != nullptr && ex->kind == JsonValue::Kind::Array) {
        for (const auto& item : ex->array_val) {
            Expression expr;
            if (const auto* e = item.find("expr")) expr = expr_from_json(*e);
            std::optional<std::string> alias;
            if (const auto* a = item.find("alias"); a != nullptr && a->kind == JsonValue::Kind::String) alias = a->str_val;
            p.expressions.emplace_back(std::move(expr), std::move(alias));
        }
    }
    if (const auto* ob = j.find("order_by"); ob != nullptr && ob->kind == JsonValue::Kind::Array) {
        for (const auto& item : ob->array_val) p.order_by.push_back(order_by_from_json(item));
    }
    if (const auto* cnt = j.find("count")) p.count = static_cast<size_t>(cnt->as_number());

    return p;
}

} // namespace

std::string serialize_plan(const PhysicalPlan& plan) {
    return sql::util::to_json(plan_to_json(plan));
}

PhysicalPlan deserialize_plan(const std::string& json) {
    return plan_from_json(sql::util::parse_json(json));
}

} // namespace sql::integration
