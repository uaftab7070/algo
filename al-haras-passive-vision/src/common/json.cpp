#include "common/json.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace ahpv::json {

Value::Value(double v) : data_(v) {
    if (!std::isfinite(v)) throw TypeError("JSON cannot represent a non-finite number");
}

bool Value::as_bool() const {
    if (!is_bool()) throw TypeError("expected boolean");
    return std::get<bool>(data_);
}

std::int64_t Value::as_int() const {
    if (is_integer()) return std::get<std::int64_t>(data_);
    if (type() == Type::Float) {
        const double d = std::get<double>(data_);
        if (std::floor(d) == d && d >= -9.2e18 && d <= 9.2e18) return static_cast<std::int64_t>(d);
    }
    throw TypeError("expected integer");
}

double Value::as_number() const {
    if (is_integer()) return static_cast<double>(std::get<std::int64_t>(data_));
    if (type() == Type::Float) return std::get<double>(data_);
    throw TypeError("expected number");
}

const std::string& Value::as_string() const {
    if (!is_string()) throw TypeError("expected string");
    return std::get<std::string>(data_);
}

const Array& Value::as_array() const {
    if (!is_array()) throw TypeError("expected array");
    return std::get<Array>(data_);
}

Array& Value::as_array() {
    if (!is_array()) throw TypeError("expected array");
    return std::get<Array>(data_);
}

const Object& Value::as_object() const {
    if (!is_object()) throw TypeError("expected object");
    return std::get<Object>(data_);
}

Object& Value::as_object() {
    if (!is_object()) throw TypeError("expected object");
    return std::get<Object>(data_);
}

const Value* Value::find(std::string_view key) const {
    if (!is_object()) return nullptr;
    for (const auto& [name, value] : std::get<Object>(data_))
        if (name == key) return &value;
    return nullptr;
}

const Value& Value::at(std::string_view key) const {
    const Value* value = find(key);
    if (!value) throw TypeError("missing field '" + std::string(key) + "'");
    return *value;
}

Value& Value::set(std::string key, Value value) {
    if (is_null()) data_ = Object{};
    auto& object = as_object();
    for (auto& [name, existing] : object) {
        if (name == key) {
            existing = std::move(value);
            return existing;
        }
    }
    object.emplace_back(std::move(key), std::move(value));
    return object.back().second;
}

namespace {

class Parser {
public:
    Parser(std::string_view text, ParseLimits limits) : text_(text), limits_(limits) {}

    Value document() {
        skip_space();
        Value value = parse_value(0);
        skip_space();
        if (pos_ != text_.size()) fail("unexpected trailing content");
        return value;
    }

private:
    std::string_view text_;
    ParseLimits limits_;
    std::size_t pos_{0};

    [[noreturn]] void fail(const std::string& message) const {
        throw ParseError("JSON " + message + " at byte " + std::to_string(pos_));
    }

    void skip_space() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
            ++pos_;
    }

    bool consume(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect_literal(std::string_view literal) {
        if (text_.substr(pos_, literal.size()) != literal) fail("invalid literal");
        pos_ += literal.size();
    }

    Value parse_value(std::size_t depth) {
        if (depth > limits_.max_depth) fail("nesting too deep");
        if (pos_ >= text_.size()) fail("unexpected end of input");
        switch (text_[pos_]) {
            case 'n': expect_literal("null"); return Value(nullptr);
            case 't': expect_literal("true"); return Value(true);
            case 'f': expect_literal("false"); return Value(false);
            case '"': return Value(parse_string());
            case '[': return parse_array(depth);
            case '{': return parse_object(depth);
            default: return parse_number();
        }
    }

    Value parse_array(std::size_t depth) {
        ++pos_;
        Array items;
        skip_space();
        if (consume(']')) return Value(std::move(items));
        for (;;) {
            skip_space();
            items.push_back(parse_value(depth + 1));
            skip_space();
            if (consume(']')) return Value(std::move(items));
            if (!consume(',')) fail("expected ',' or ']'");
        }
    }

    Value parse_object(std::size_t depth) {
        ++pos_;
        Object members;
        skip_space();
        if (consume('}')) return Value(std::move(members));
        for (;;) {
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != '"') fail("expected object key");
            std::string key = parse_string();
            for (const auto& member : members)
                if (member.first == key) fail("duplicate key '" + key + "'");
            skip_space();
            if (!consume(':')) fail("expected ':'");
            skip_space();
            Value value = parse_value(depth + 1);
            members.emplace_back(std::move(key), std::move(value));
            skip_space();
            if (consume('}')) return Value(std::move(members));
            if (!consume(',')) fail("expected ',' or '}'");
        }
    }

    unsigned hex4() {
        if (pos_ + 4 > text_.size()) fail("truncated unicode escape");
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
            else fail("invalid unicode escape");
        }
        return value;
    }

    static void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    // Validates one raw UTF-8 sequence starting at pos_ and appends it.
    void copy_utf8(std::string& out) {
        const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text_[i]); };
        const unsigned char lead = byte(pos_);
        int extra = 0;
        unsigned cp = 0;
        if (lead >= 0xC2 && lead <= 0xDF) { extra = 1; cp = lead & 0x1F; }
        else if (lead >= 0xE0 && lead <= 0xEF) { extra = 2; cp = lead & 0x0F; }
        else if (lead >= 0xF0 && lead <= 0xF4) { extra = 3; cp = lead & 0x07; }
        else fail("invalid UTF-8");
        if (pos_ + static_cast<std::size_t>(extra) >= text_.size()) fail("truncated UTF-8");
        for (int i = 1; i <= extra; ++i) {
            const unsigned char next = byte(pos_ + static_cast<std::size_t>(i));
            if ((next & 0xC0) != 0x80) fail("invalid UTF-8");
            cp = (cp << 6) | (next & 0x3F);
        }
        if ((extra == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
            (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)))
            fail("invalid UTF-8");
        out.append(text_.substr(pos_, static_cast<std::size_t>(extra) + 1));
        pos_ += static_cast<std::size_t>(extra) + 1;
    }

    std::string parse_string() {
        ++pos_;  // opening quote
        std::string out;
        for (;;) {
            if (pos_ >= text_.size()) fail("unterminated string");
            if (out.size() > limits_.max_string_bytes) fail("string too long");
            const unsigned char c = static_cast<unsigned char>(text_[pos_]);
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (c < 0x20) fail("unescaped control character in string");
            if (c >= 0x80) {
                copy_utf8(out);
                continue;
            }
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                ++pos_;
                continue;
            }
            ++pos_;
            if (pos_ >= text_.size()) fail("truncated escape");
            const char e = text_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (!(consume('\\') && consume('u'))) fail("unpaired surrogate");
                        const unsigned low = hex4();
                        if (low < 0xDC00 || low > 0xDFFF) fail("invalid surrogate pair");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail("unpaired surrogate");
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: fail("invalid escape");
            }
        }
    }

    Value parse_number() {
        const std::size_t start = pos_;
        bool integral = true;
        consume('-');
        if (consume('0')) {
        } else if (pos_ < text_.size() && text_[pos_] >= '1' && text_[pos_] <= '9') {
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        } else {
            fail("invalid value");
        }
        if (consume('.')) {
            integral = false;
            if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) fail("invalid number");
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            integral = false;
            ++pos_;
            if (!consume('+')) consume('-');
            if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) fail("invalid number");
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        const std::string literal(text_.substr(start, pos_ - start));
        if (integral) {
            errno = 0;
            char* end = nullptr;
            const long long v = std::strtoll(literal.c_str(), &end, 10);
            if (errno == 0 && end && *end == '\0') return Value(static_cast<std::int64_t>(v));
        }
        const double d = std::strtod(literal.c_str(), nullptr);
        if (!std::isfinite(d)) fail("number out of range");
        return Value(d);
    }
};

void dump_into(const Value& value, std::string& out) {
    switch (value.type()) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += value.as_bool() ? "true" : "false"; break;
        case Value::Type::Integer: out += std::to_string(value.as_int()); break;
        case Value::Type::Float: {
            char buffer[40];
            std::snprintf(buffer, sizeof buffer, "%.12g", value.as_number());
            out += buffer;
            // Keep floats recognisable as non-integers on re-parse.
            if (!std::strpbrk(buffer, ".eE")) out += ".0";
            break;
        }
        case Value::Type::String: out += quote(value.as_string()); break;
        case Value::Type::Array: {
            out.push_back('[');
            bool first = true;
            for (const auto& item : value.as_array()) {
                if (!first) out.push_back(',');
                first = false;
                dump_into(item, out);
            }
            out.push_back(']');
            break;
        }
        case Value::Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& [key, item] : value.as_object()) {
                if (!first) out.push_back(',');
                first = false;
                out += quote(key);
                out.push_back(':');
                dump_into(item, out);
            }
            out.push_back('}');
            break;
        }
    }
}

}  // namespace

Value parse(std::string_view text, ParseLimits limits) { return Parser(text, limits).document(); }

std::string dump(const Value& value) {
    std::string out;
    dump_into(value, out);
    return out;
}

std::string quote(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace ahpv::json
