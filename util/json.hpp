#pragma once

#include <string>
#include <utility>
#include <vector>

namespace sql::util {

struct JsonValue;

// An insertion-ordered string -> JsonValue map, backed by a vector of pairs
// rather than std::unordered_map: unordered_map requires its mapped type to
// be complete at the point the container is instantiated, which JsonValue
// isn't while it's still defining itself below. vector (like list/
// forward_list) is explicitly permitted to hold an incomplete type here --
// the same relaxation JsonValue::array_val already relies on for itself.
class JsonObject {
public:
    using Entry = std::pair<std::string, JsonValue>;

    JsonValue& operator[](const std::string& key);
    const JsonValue* find(const std::string& key) const;

    std::vector<Entry>::iterator begin();
    std::vector<Entry>::iterator end();
    std::vector<Entry>::const_iterator begin() const;
    std::vector<Entry>::const_iterator end() const;

private:
    std::vector<Entry> entries_;
};

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
    JsonObject object_val;

    const JsonValue* find(const std::string& key) const {
        if (kind != Kind::Object) return nullptr;
        return object_val.find(key);
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
