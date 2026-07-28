#include "statistics_loader.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace sql::statistics {

namespace {

// ── Minimal JSON parser ───────────────────────────────────────────────────────
//
// Just enough JSON to read the statistics file shape documented in
// statistics_loader.hpp: objects, arrays, strings, numbers, true/false/null.
// Not a general-purpose library -- kept private to this translation unit,
// matching the project's zero-external-dependency approach (same rationale
// as the hand-written SQL lexer/parser).

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
};

class JsonParser {
public:
    // Stored by value, not reference: callers often pass a temporary
    // (e.g. JsonParser(buffer.str())), and a reference member bound to
    // that temporary would dangle the instant the constructor returns --
    // reference lifetime extension only applies when a temporary binds
    // directly to a named reference variable, not through a constructor
    // parameter stashed in a member.
    explicit JsonParser(std::string text) : text_(std::move(text)) {}

    JsonValue parse() {
        skip_whitespace();
        JsonValue v = parse_value();
        skip_whitespace();
        return v;
    }

private:
    std::string text_;
    size_t pos_ = 0;

    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
    char advance() { return pos_ < text_.size() ? text_[pos_++] : '\0'; }

    void skip_whitespace() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    void expect(char c) {
        if (peek() != c) {
            throw std::runtime_error(std::string("json: expected '") + c + "' at position " + std::to_string(pos_));
        }
        advance();
    }

    JsonValue parse_value() {
        skip_whitespace();
        switch (peek()) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': return parse_string_value();
            case 't':
            case 'f': return parse_bool();
            case 'n': return parse_null();
            default: return parse_number();
        }
    }

    JsonValue parse_object() {
        JsonValue v;
        v.kind = JsonValue::Kind::Object;
        expect('{');
        skip_whitespace();
        if (peek() == '}') {
            advance();
            return v;
        }
        for (;;) {
            skip_whitespace();
            std::string key = parse_raw_string();
            skip_whitespace();
            expect(':');
            v.object_val[key] = parse_value();
            skip_whitespace();
            if (peek() == ',') {
                advance();
                continue;
            }
            break;
        }
        skip_whitespace();
        expect('}');
        return v;
    }

    JsonValue parse_array() {
        JsonValue v;
        v.kind = JsonValue::Kind::Array;
        expect('[');
        skip_whitespace();
        if (peek() == ']') {
            advance();
            return v;
        }
        for (;;) {
            v.array_val.push_back(parse_value());
            skip_whitespace();
            if (peek() == ',') {
                advance();
                skip_whitespace();
                continue;
            }
            break;
        }
        skip_whitespace();
        expect(']');
        return v;
    }

    std::string parse_raw_string() {
        expect('"');
        std::string s;
        for (;;) {
            if (pos_ >= text_.size()) throw std::runtime_error("json: unterminated string");
            char c = advance();
            if (c == '"') break;
            if (c == '\\') {
                char esc = advance();
                switch (esc) {
                    case '"': s.push_back('"'); break;
                    case '\\': s.push_back('\\'); break;
                    case '/': s.push_back('/'); break;
                    case 'n': s.push_back('\n'); break;
                    case 't': s.push_back('\t'); break;
                    default: s.push_back(esc); break;
                }
            } else {
                s.push_back(c);
            }
        }
        return s;
    }

    JsonValue parse_string_value() {
        JsonValue v;
        v.kind = JsonValue::Kind::String;
        v.str_val = parse_raw_string();
        return v;
    }

    JsonValue parse_bool() {
        JsonValue v;
        v.kind = JsonValue::Kind::Bool;
        if (text_.compare(pos_, 4, "true") == 0) {
            v.bool_val = true;
            pos_ += 4;
        } else if (text_.compare(pos_, 5, "false") == 0) {
            v.bool_val = false;
            pos_ += 5;
        } else {
            throw std::runtime_error("json: invalid literal at position " + std::to_string(pos_));
        }
        return v;
    }

    JsonValue parse_null() {
        if (text_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return JsonValue{};
        }
        throw std::runtime_error("json: invalid literal at position " + std::to_string(pos_));
    }

    JsonValue parse_number() {
        size_t start = pos_;
        if (peek() == '-') advance();
        while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
        if (peek() == '.') {
            advance();
            while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
        }
        if (peek() == 'e' || peek() == 'E') {
            advance();
            if (peek() == '+' || peek() == '-') advance();
            while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
        }
        if (pos_ == start) throw std::runtime_error("json: invalid number at position " + std::to_string(pos_));

        JsonValue v;
        v.kind = JsonValue::Kind::Number;
        v.num_val = std::stod(text_.substr(start, pos_ - start));
        return v;
    }
};

TableStats table_stats_from_json(const JsonValue& root) {
    TableStats stats;
    if (const auto* rc = root.find("row_count")) stats.row_count = rc->as_number();
    if (const auto* pc = root.find("page_count")) stats.page_count = pc->as_number();

    const auto* cols = root.find("columns");
    if (cols == nullptr || cols->kind != JsonValue::Kind::Object) return stats;

    for (const auto& [name, col_val] : cols->object_val) {
        ColumnStats cs;
        if (const auto* dc = col_val.find("distinct_count")) cs.distinct_count = dc->as_number();
        if (const auto* nf = col_val.find("null_fraction")) cs.null_fraction = nf->as_number();
        if (const auto* mn = col_val.find("min")) cs.min_value = mn->as_number();
        if (const auto* mx = col_val.find("max")) cs.max_value = mx->as_number();

        if (const auto* hist = col_val.find("histogram")) {
            if (const auto* buckets = hist->find("buckets"); buckets != nullptr && buckets->kind == JsonValue::Kind::Array) {
                Histogram h;
                for (const auto& b : buckets->array_val) {
                    HistogramBucket bucket{};
                    if (const auto* lo = b.find("lower")) bucket.lower = lo->as_number();
                    if (const auto* hi = b.find("upper")) bucket.upper = hi->as_number();
                    if (const auto* fr = b.find("frequency")) bucket.frequency = fr->as_number();
                    h.buckets.push_back(bucket);
                }
                cs.histogram = std::move(h);
            }
        }

        stats.columns[name] = std::move(cs);
    }

    return stats;
}

} // namespace

TableStats load_table_stats_from_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("statistics: could not open file: " + path);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();

    JsonParser parser(buffer.str());
    JsonValue root = parser.parse();
    return table_stats_from_json(root);
}

StatisticsCatalog load_catalog_from_directory(const std::string& dir_path) {
    StatisticsCatalog catalog;

    if (!std::filesystem::exists(dir_path) || !std::filesystem::is_directory(dir_path)) {
        throw std::runtime_error("statistics: not a directory: " + dir_path);
    }

    for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;

        std::string table_name = entry.path().stem().string();
        TableStats stats = load_table_stats_from_file(entry.path().string());
        catalog.register_table(table_name, std::move(stats));
    }

    return catalog;
}

} // namespace sql::statistics
