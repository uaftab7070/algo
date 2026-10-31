#pragma once
// Strict RFC 8259 JSON value, parser, and deterministic serializer.
// Integers are kept exactly as int64 (nanosecond timestamps exceed 2^53).
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ahpv::json {

class Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;  // Insertion-ordered.

struct ParseError : std::runtime_error {
    explicit ParseError(const std::string& message) : std::runtime_error(message) {}
};
struct TypeError : std::runtime_error {
    explicit TypeError(const std::string& message) : std::runtime_error(message) {}
};

class Value {
public:
    enum class Type { Null, Bool, Integer, Float, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool v) : data_(v) {}
    template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Value(T v) : data_(static_cast<std::int64_t>(v)) {
        if constexpr (std::is_unsigned_v<T> && sizeof(T) >= sizeof(std::int64_t)) {
            if (v > static_cast<T>(INT64_MAX)) throw TypeError("unsigned value exceeds int64 range");
        }
    }
    Value(double v);
    Value(const char* v) : data_(std::string(v)) {}
    Value(std::string v) : data_(std::move(v)) {}
    Value(std::string_view v) : data_(std::string(v)) {}
    Value(json::Array v) : data_(std::move(v)) {}
    Value(json::Object v) : data_(std::move(v)) {}

    Type type() const { return static_cast<Type>(data_.index()); }
    bool is_null() const { return type() == Type::Null; }
    bool is_bool() const { return type() == Type::Bool; }
    bool is_integer() const { return type() == Type::Integer; }
    bool is_number() const { return type() == Type::Integer || type() == Type::Float; }
    bool is_string() const { return type() == Type::String; }
    bool is_array() const { return type() == Type::Array; }
    bool is_object() const { return type() == Type::Object; }

    bool as_bool() const;
    std::int64_t as_int() const;
    double as_number() const;
    const std::string& as_string() const;
    const json::Array& as_array() const;
    json::Array& as_array();
    const json::Object& as_object() const;
    json::Object& as_object();

    const Value* find(std::string_view key) const;
    // Typed lookups throw TypeError naming the key when absent or mistyped.
    const Value& at(std::string_view key) const;
    Value& set(std::string key, Value value);
    void push(Value value) { as_array().push_back(std::move(value)); }

    bool operator==(const Value& other) const { return data_ == other.data_; }
    bool operator!=(const Value& other) const { return !(*this == other); }

private:
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, json::Array, json::Object> data_{nullptr};
};

struct ParseLimits {
    std::size_t max_depth{64};
    std::size_t max_string_bytes{1 << 20};
};

// Rejects trailing content, duplicate keys, invalid UTF-8 escapes and NaN/Infinity.
Value parse(std::string_view text, ParseLimits limits = {});
std::string dump(const Value& value);
std::string quote(std::string_view text);

}  // namespace ahpv::json
