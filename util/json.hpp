#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace sql::util {

// ── Minimal JSON value + parser + writer ────────────────────────────────────
//
// Just enough JSON to round-trip the statistics fixtures (statistics/) and
// serialized plan cache values (integration/) -- not a general-purpose
// library. Kept dependency-free, matching this project's established
// zero-external-dependency approach (same rationale as the hand-written SQL
// lexer/parser).
struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool bool_val = false;
    double num_val = 0.0;
    std::string str_val;
    std::vector<JsonValue> array_val;
    std::unordered_map<std::string, JsonValue> object_val;

    const JsonValue* find(const std::string& key) const {
        if (kind != Kind::Object) return nullptr;
        auto it = object_val.find(key);
        return it == object_val.end() ? nullptr : &it->second;
    }

    double as_number(double default_val = 0.0) const {
        return kind == Kind::Number ? num_val : default_val;
    }

    std::string as_string(const std::string& default_val = "") const {
        return kind == Kind::String ? str_val : default_val;
    }

    bool as_bool(bool default_val = false) const {
        return kind == Kind::Bool ? bool_val : default_val;
    }

    static JsonValue make_null() { return JsonValue{}; }

    static JsonValue make_bool(bool v) {
        JsonValue j;
        j.kind = Kind::Bool;
        j.bool_val = v;
        return j;
    }

    static JsonValue make_number(double v) {
        JsonValue j;
        j.kind = Kind::Number;
        j.num_val = v;
        return j;
    }

    static JsonValue make_string(std::string v) {
        JsonValue j;
        j.kind = Kind::String;
        j.str_val = std::move(v);
        return j;
    }

    static JsonValue make_array() {
        JsonValue j;
        j.kind = Kind::Array;
        return j;
    }

    static JsonValue make_object() {
        JsonValue j;
        j.kind = Kind::Object;
        return j;
    }
};

/// Parses `text` as a single JSON value. Throws std::runtime_error on
/// malformed input.
JsonValue parse_json(const std::string& text);

/// Serializes `value` back to JSON text (compact, no pretty-printing --
/// this is written for machine round-tripping, not human editing).
std::string to_json(const JsonValue& value);

} // namespace sql::util
