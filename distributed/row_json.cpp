#include "row_json.hpp"

namespace sql::distributed {

using sql::parser::Literal;
using sql::util::JsonValue;

namespace {

JsonValue literal_to_json(const Literal& v) {
    JsonValue j = JsonValue::make_object();
    switch (v.kind) {
        case Literal::Kind::Integer:
            j.object_val["k"] = JsonValue::make_string("i");
            j.object_val["v"] = JsonValue::make_number(static_cast<double>(v.int_val));
            break;
        case Literal::Kind::Float:
            j.object_val["k"] = JsonValue::make_string("f");
            j.object_val["v"] = JsonValue::make_number(v.float_val);
            break;
        case Literal::Kind::Str:
            j.object_val["k"] = JsonValue::make_string("s");
            j.object_val["v"] = JsonValue::make_string(v.str_val);
            break;
        case Literal::Kind::Boolean:
            j.object_val["k"] = JsonValue::make_string("b");
            j.object_val["v"] = JsonValue::make_bool(v.bool_val);
            break;
        case Literal::Kind::Null:
            j.object_val["k"] = JsonValue::make_string("n");
            break;
    }
    return j;
}

Literal literal_from_json(const JsonValue& j) {
    const auto* k = j.find("k");
    std::string kind = k != nullptr ? k->as_string() : "n";
    const auto* v = j.find("v");
    if (kind == "i") return Literal::integer(v != nullptr ? static_cast<int64_t>(v->as_number()) : 0);
    if (kind == "f") return Literal::floating(v != nullptr ? v->as_number() : 0.0);
    if (kind == "s") return Literal::str(v != nullptr ? v->as_string() : "");
    if (kind == "b") return Literal::boolean(v != nullptr && v->as_bool());
    return Literal::null();
}

} // namespace

JsonValue rows_to_json(const std::vector<sql::storage::Row>& rows) {
    JsonValue arr = JsonValue::make_array();
    for (const auto& row : rows) {
        JsonValue row_json = JsonValue::make_array();
        for (const auto& value : row) row_json.array_val.push_back(literal_to_json(value));
        arr.array_val.push_back(std::move(row_json));
    }
    return arr;
}

std::vector<sql::storage::Row> rows_from_json(const JsonValue& value) {
    std::vector<sql::storage::Row> rows;
    if (value.kind != JsonValue::Kind::Array) return rows;
    for (const auto& row_json : value.array_val) {
        sql::storage::Row row;
        if (row_json.kind == JsonValue::Kind::Array) {
            for (const auto& v : row_json.array_val) row.push_back(literal_from_json(v));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace sql::distributed
