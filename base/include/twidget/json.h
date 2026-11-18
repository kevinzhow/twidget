// twidget::Json: the small JSON value TwidgetCore and the card host pass messages in (SPEC S11.1).
//
// Parsing and writing are yyjson's (MIT, pinned in base/base.cmake). On top of it this type adds what SPEC S11.1
// asks for and yyjson does not do by itself:
//   - a nesting limit (kMaxDepth) on hostile input;
//   - lone UTF-16 surrogates in `\u` escapes kept as their 3-byte WTF-8 form (SPEC "Conventions"), and written back
//     as `\uXXXX` escapes, so a dump is always valid UTF-8 JSON;
//   - duplicate object keys: the last one wins (its value, at the first one's place);
//   - numbers written shortest round-trip, locale independent, integral values without a fraction.
// Objects keep their members in insertion order: a renderer message's `type` member comes first in its dump.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace twidget {

class Json {
public:
    using Array = std::vector<Json>;
    using Member = std::pair<std::string, Json>;
    using Object = std::vector<Member>;
    enum class Type { Null, Bool, Number, String, Array, Object };

    /// Deepest nesting of arrays and objects parse() accepts (SPEC S11.1: "a nesting depth limit (e.g. 256)").
    static constexpr int kMaxDepth = 256;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : v_(v) {}
    Json(int v) : v_(double(v)) {}
    Json(unsigned v) : v_(double(v)) {}
    Json(long v) : v_(double(v)) {}
    Json(unsigned long v) : v_(double(v)) {}
    Json(long long v) : v_(double(v)) {}
    Json(unsigned long long v) : v_(double(v)) {}
    Json(double v) : v_(v) {}
    Json(float v) : v_(double(v)) {}
    Json(const char* v) : v_(std::string(v ? v : "")) {}
    Json(std::string v) : v_(std::move(v)) {}
    Json(std::string_view v) : v_(std::string(v)) {}
    Json(Array v) : v_(std::move(v)) {}
    Json(Object v) : v_(std::move(v)) {}

    static Json object() { return Json(Object{}); }
    static Json array() { return Json(Array{}); }

    Type type() const { return Type(v_.index()); }
    bool isNull() const { return type() == Type::Null; }
    bool isBool() const { return type() == Type::Bool; }
    bool isNumber() const { return type() == Type::Number; }
    bool isString() const { return type() == Type::String; }
    bool isArray() const { return type() == Type::Array; }
    bool isObject() const { return type() == Type::Object; }

    /// The value, or `def` when it is of another type.
    const std::string& asString() const;
    double asNumber(double def = 0) const;
    /// A number truncated toward zero and clamped to int64; `def` when not a number (or NaN).
    int64_t asInt(int64_t def = 0) const;
    bool asBool(bool def = false) const;

    /// Members and items; empty for other types.
    const Array& arr() const;
    const Object& obj() const;
    Array& arr();
    Object& obj();
    /// Items of an array, members of an object, bytes of a string; 0 otherwise.
    size_t size() const;
    bool empty() const { return size() == 0; }

    /// The member `key`, or null (a shared constant) when there is none or this is no object.
    const Json& operator[](std::string_view key) const;
    const Json& operator[](const char* key) const { return (*this)[std::string_view(key)]; }
    const Json& operator[](const std::string& key) const { return (*this)[std::string_view(key)]; }
    /// The item `index`, or null.
    const Json& operator[](size_t index) const;
    /// The member `key`, or nullptr.
    const Json* find(std::string_view key) const;
    Json* find(std::string_view key);

    /// Sets member `key` (replacing an existing one in place); this becomes an object if it was not one. Returns *this.
    Json& set(std::string_view key, Json value);
    /// Removes member `key`; whether there was one.
    bool erase(std::string_view key);
    /// Appends an item; this becomes an array if it was not one. Returns *this.
    Json& push(Json value);

    /// Compact JSON text, valid UTF-8 (see the header comment).
    std::string dump() const;

    /// Strict RFC 8259 text → value; nullopt on any error or nesting deeper than kMaxDepth.
    static std::optional<Json> parse(std::string_view text);

    /// `v` as JSON writes it: shortest round-trip form, '.' as the decimal point, integral values without a
    /// fraction, -0 as 0; a non-finite value as "0" (callers build CSS and JSON text from it).
    static std::string number(double v);
    /// `s` as a JSON string literal, quotes included (lone surrogates as escapes, invalid UTF-8 as U+FFFD).
    static std::string quote(std::string_view s);
    /// The number at the start of `s` (JSON number syntax, plus a leading '+' or '.'), locale independent; 0 when
    /// there is none.
    static double parseDouble(const char* s);

    bool operator==(const Json& o) const { return v_ == o.v_; }
    bool operator!=(const Json& o) const { return !(*this == o); }

private:
    std::variant<std::monostate, bool, double, std::string, Array, Object> v_;
};

} // namespace twidget
