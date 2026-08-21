#include "json.hpp"

#include <cctype>
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace sql::util {

namespace {

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

void write_escaped_string(const std::string& s, std::ostream& os) {
    os << '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\n': os << "\\n"; break;
            case '\t': os << "\\t"; break;
            case '\r': os << "\\r"; break;
            default:
                if (c < 0x20) {
                    static constexpr char kHex[] = "0123456789abcdef";
                    os << "\\u00" << kHex[(c >> 4) & 0xF] << kHex[c & 0xF];
                } else {
                    os << static_cast<char>(c);
                }
        }
    }
    os << '"';
}

void write_json(const JsonValue& v, std::ostream& os) {
    switch (v.kind) {
        case JsonValue::Kind::Null:
            os << "null";
            break;
        case JsonValue::Kind::Bool:
            os << (v.bool_val ? "true" : "false");
            break;
        case JsonValue::Kind::Number: {
            // %.17g round-trips any IEEE double exactly; ostream's default
            // precision (6 significant digits) would silently truncate row
            // counts and cost values on every serialize/deserialize cycle.
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g", v.num_val);
            os << buf;
            break;
        }
        case JsonValue::Kind::String:
            write_escaped_string(v.str_val, os);
            break;
        case JsonValue::Kind::Array: {
            os << '[';
            for (size_t i = 0; i < v.array_val.size(); ++i) {
                if (i > 0) os << ',';
                write_json(v.array_val[i], os);
            }
            os << ']';
            break;
        }
        case JsonValue::Kind::Object: {
            os << '{';
            bool first = true;
            for (const auto& [key, val] : v.object_val) {
                if (!first) os << ',';
                first = false;
                write_escaped_string(key, os);
                os << ':';
                write_json(val, os);
            }
            os << '}';
            break;
        }
    }
}

} // namespace

JsonValue parse_json(const std::string& text) {
    JsonParser parser(text);
    return parser.parse();
}

std::string to_json(const JsonValue& value) {
    std::ostringstream os;
    write_json(value, os);
    return os.str();
}

} // namespace sql::util
