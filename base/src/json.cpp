// twidget::Json over yyjson (SPEC S11.1). See json.h for what this adds to yyjson; each addition is marked below.
#include "twidget/json.h"

#include <yyjson.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace twidget {

namespace {

const Json& null_value() {
    static const Json n;
    return n;
}
const std::string& empty_string() {
    static const std::string s;
    return s;
}
const Json::Array& empty_array() {
    static const Json::Array a;
    return a;
}
const Json::Object& empty_object() {
    static const Json::Object o;
    return o;
}

// ── WTF-8 (SPEC "Conventions": a lone UTF-16 surrogate is kept as its 3-byte form) ──
//
// yyjson refuses a `\u` escape of a lone surrogate, and writes strings byte for byte. The two passes below are the
// only JSON text handling of our own: they rewrite surrogate escapes into raw WTF-8 bytes before yyjson reads the
// text (with YYJSON_READ_ALLOW_INVALID_UNICODE), and raw WTF-8 surrogates back into escapes after it writes.

/// Length of the well-formed UTF-8 sequence at `s[i]` (1-4), or of a WTF-8 surrogate (3, `*surrogate` set), or 0 when
/// the bytes there are neither. RFC 3629 §4 syntax, with ED A0..BF allowed for surrogates.
size_t sequence_at(const unsigned char* s, size_t n, size_t i, bool* surrogate) {
    *surrogate = false;
    unsigned char c = s[i];
    if (c < 0x80) return 1;
    auto cont = [&](size_t k) { return i + k < n && (s[i + k] & 0xC0) == 0x80; };
    if (c >= 0xC2 && c <= 0xDF) return cont(1) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        if (!cont(1) || !cont(2)) return 0;
        unsigned char c1 = s[i + 1];
        if (c == 0xE0 && c1 < 0xA0) return 0;  // overlong
        if (c == 0xED && c1 >= 0xA0) *surrogate = true;
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (!cont(1) || !cont(2) || !cont(3)) return 0;
        unsigned char c1 = s[i + 1];
        if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) return 0;
        return 4;
    }
    return 0;
}

bool valid_wtf8(std::string_view s) {
    auto* p = reinterpret_cast<const unsigned char*>(s.data());
    bool sur;
    for (size_t i = 0; i < s.size();) {
        size_t k = sequence_at(p, s.size(), i, &sur);
        if (!k) return false;
        i += k;
    }
    return true;
}

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// The code unit of a `\uXXXX` escape at text[i] (the backslash), or -1.
int escape_at(std::string_view t, size_t i) {
    if (i + 6 > t.size() || t[i] != '\\' || t[i + 1] != 'u') return -1;
    int v = 0;
    for (size_t k = 2; k < 6; k++) {
        int h = hex(t[i + k]);
        if (h < 0) return -1;
        v = v * 16 + h;
    }
    return v;
}

/// `text` with every escape of a lone surrogate replaced by its WTF-8 bytes; nullopt when there is none (the text
/// goes to yyjson as it is). Escape sequences are walked pairwise, so `\\ud800` (an escaped backslash) is left alone.
std::optional<std::string> rewrite_lone_surrogates(std::string_view t) {
    std::optional<std::string> out;
    size_t copied = 0;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] != '\\') continue;
        int u = escape_at(t, i);
        if (u < 0) {
            i++;  // skip the escaped character
            continue;
        }
        if (u < 0xD800 || u > 0xDFFF) {
            i += 5;
            continue;
        }
        if (u <= 0xDBFF) {
            int lo = escape_at(t, i + 6);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {  // a proper pair: yyjson decodes it
                i += 11;
                continue;
            }
        }
        if (!out) out.emplace();
        out->append(t.substr(copied, i - copied));
        out->push_back(char(0xE0 | (u >> 12)));
        out->push_back(char(0x80 | ((u >> 6) & 0x3F)));
        out->push_back(char(0x80 | (u & 0x3F)));
        copied = i + 6;
        i += 5;
    }
    if (out) out->append(t.substr(copied));
    return out;
}

/// yyjson's output (or a string) made valid UTF-8: WTF-8 surrogates as `\uXXXX`, other invalid bytes as `�`.
/// Every byte that needs it is inside a string literal (the rest of the text is ASCII).
void escape_invalid(std::string& s) {
    auto* p = reinterpret_cast<const unsigned char*>(s.data());
    size_t i = 0;
    bool sur;
    while (i < s.size()) {
        if (p[i] < 0x80) {
            i++;
            continue;
        }
        size_t k = sequence_at(p, s.size(), i, &sur);
        if (k && !sur) {
            i += k;
            continue;
        }
        break;
    }
    if (i == s.size()) return;  // the common case: nothing to do
    std::string out(s, 0, i);
    static const char digits[] = "0123456789abcdef";
    while (i < s.size()) {
        size_t k = sequence_at(p, s.size(), i, &sur);
        if (k && !sur) {
            out.append(s, i, k);
            i += k;
            continue;
        }
        unsigned u = 0xFFFD;
        if (k) u = ((p[i] & 0x0F) << 12) | ((p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F);
        out += "\\u";
        out += digits[(u >> 12) & 15];
        out += digits[(u >> 8) & 15];
        out += digits[(u >> 4) & 15];
        out += digits[u & 15];
        i += k ? k : 1;
    }
    s = std::move(out);
}

// ── yyjson ↔ Json ──

bool from_yyjson(yyjson_val* v, Json& out, int depth) {
    switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_NULL: out = Json(); return true;
    case YYJSON_TYPE_BOOL: out = Json(yyjson_get_bool(v)); return true;
    case YYJSON_TYPE_NUM: out = Json(yyjson_get_num(v)); return true;
    case YYJSON_TYPE_STR: {
        std::string_view s(yyjson_get_str(v), yyjson_get_len(v));
        // Read with ALLOW_INVALID_UNICODE only after the surrogate rewrite: still refuse other invalid UTF-8.
        if (!valid_wtf8(s)) return false;
        out = Json(std::string(s));
        return true;
    }
    case YYJSON_TYPE_ARR: {
        if (depth >= Json::kMaxDepth) return false;
        Json::Array items;
        items.reserve(yyjson_arr_size(v));
        yyjson_arr_iter it = yyjson_arr_iter_with(v);
        while (yyjson_val* item = yyjson_arr_iter_next(&it)) {
            items.emplace_back();
            if (!from_yyjson(item, items.back(), depth + 1)) return false;
        }
        out = Json(std::move(items));
        return true;
    }
    case YYJSON_TYPE_OBJ: {
        if (depth >= Json::kMaxDepth) return false;
        out = Json::object();
        yyjson_obj_iter it = yyjson_obj_iter_with(v);
        while (yyjson_val* key = yyjson_obj_iter_next(&it)) {
            std::string_view k(yyjson_get_str(key), yyjson_get_len(key));
            if (!valid_wtf8(k)) return false;
            Json value;
            if (!from_yyjson(yyjson_obj_iter_get_val(key), value, depth + 1)) return false;
            out.set(k, std::move(value));  // a duplicate key: the last value wins
        }
        return true;
    }
    default: return false;
    }
}

yyjson_mut_val* to_yyjson(yyjson_mut_doc* doc, const Json& j) {
    switch (j.type()) {
    case Json::Type::Null: return yyjson_mut_null(doc);
    case Json::Type::Bool: return yyjson_mut_bool(doc, j.asBool());
    case Json::Type::Number: {
        // Integral values (exactly representable ones) as integers: "1", not yyjson's "1.0" for a real.
        double d = j.asNumber();
        if (std::trunc(d) == d && std::fabs(d) <= 9007199254740992.0) return yyjson_mut_sint(doc, int64_t(d));
        return yyjson_mut_real(doc, d);
    }
    case Json::Type::String: return yyjson_mut_strn(doc, j.asString().data(), j.asString().size());
    case Json::Type::Array: {
        yyjson_mut_val* a = yyjson_mut_arr(doc);
        for (const Json& item : j.arr()) yyjson_mut_arr_append(a, to_yyjson(doc, item));
        return a;
    }
    case Json::Type::Object: {
        yyjson_mut_val* o = yyjson_mut_obj(doc);
        for (const auto& [k, v] : j.obj()) yyjson_mut_obj_add(o, yyjson_mut_strn(doc, k.data(), k.size()), to_yyjson(doc, v));
        return o;
    }
    }
    return yyjson_mut_null(doc);
}

struct MutDoc {
    yyjson_mut_doc* d = yyjson_mut_doc_new(nullptr);
    ~MutDoc() { yyjson_mut_doc_free(d); }
};

/// yyjson's text of `root`, made valid UTF-8 (escape_invalid).
std::string write(yyjson_mut_doc* d, yyjson_mut_val* root) {
    yyjson_mut_doc_set_root(d, root);
    size_t len = 0;
    char* text = yyjson_mut_write(d, YYJSON_WRITE_ALLOW_INVALID_UNICODE | YYJSON_WRITE_INF_AND_NAN_AS_NULL, &len);
    if (!text) return "null";
    std::string out(text, len);
    std::free(text);
    escape_invalid(out);
    return out;
}

} // namespace

const std::string& Json::asString() const {
    auto* s = std::get_if<std::string>(&v_);
    return s ? *s : empty_string();
}
double Json::asNumber(double def) const {
    auto* d = std::get_if<double>(&v_);
    return d ? *d : def;
}
int64_t Json::asInt(int64_t def) const {
    auto* d = std::get_if<double>(&v_);
    if (!d || std::isnan(*d)) return def;
    if (*d >= 9.2233720368547758e18) return INT64_MAX;
    if (*d <= -9.2233720368547758e18) return INT64_MIN;
    return int64_t(*d);
}
bool Json::asBool(bool def) const {
    auto* b = std::get_if<bool>(&v_);
    return b ? *b : def;
}

const Json::Array& Json::arr() const {
    auto* a = std::get_if<Array>(&v_);
    return a ? *a : empty_array();
}
const Json::Object& Json::obj() const {
    auto* o = std::get_if<Object>(&v_);
    return o ? *o : empty_object();
}
Json::Array& Json::arr() {
    if (!isArray()) v_ = Array{};
    return std::get<Array>(v_);
}
Json::Object& Json::obj() {
    if (!isObject()) v_ = Object{};
    return std::get<Object>(v_);
}
size_t Json::size() const {
    switch (type()) {
    case Type::Array: return std::get<Array>(v_).size();
    case Type::Object: return std::get<Object>(v_).size();
    case Type::String: return std::get<std::string>(v_).size();
    default: return 0;
    }
}

const Json* Json::find(std::string_view key) const {
    auto* o = std::get_if<Object>(&v_);
    if (!o) return nullptr;
    for (const auto& [k, v] : *o)
        if (k == key) return &v;
    return nullptr;
}
Json* Json::find(std::string_view key) {
    return const_cast<Json*>(static_cast<const Json*>(this)->find(key));
}
const Json& Json::operator[](std::string_view key) const {
    const Json* v = find(key);
    return v ? *v : null_value();
}
const Json& Json::operator[](size_t index) const {
    auto* a = std::get_if<Array>(&v_);
    return a && index < a->size() ? (*a)[index] : null_value();
}

Json& Json::set(std::string_view key, Json value) {
    Object& o = obj();
    for (auto& [k, v] : o)
        if (k == key) {
            v = std::move(value);
            return *this;
        }
    o.emplace_back(std::string(key), std::move(value));
    return *this;
}
bool Json::erase(std::string_view key) {
    auto* o = std::get_if<Object>(&v_);
    if (!o) return false;
    for (auto it = o->begin(); it != o->end(); ++it)
        if (it->first == key) {
            o->erase(it);
            return true;
        }
    return false;
}
Json& Json::push(Json value) {
    arr().push_back(std::move(value));
    return *this;
}

std::string Json::dump() const {
    MutDoc doc;
    return write(doc.d, to_yyjson(doc.d, *this));
}

std::optional<Json> Json::parse(std::string_view text) {
    std::optional<std::string> rewritten = rewrite_lone_surrogates(text);
    std::string_view input = rewritten ? std::string_view(*rewritten) : text;
    yyjson_read_flag flags = rewritten ? YYJSON_READ_ALLOW_INVALID_UNICODE : 0;
    yyjson_read_err err;
    yyjson_doc* doc = yyjson_read_opts(const_cast<char*>(input.data()), input.size(), flags, nullptr, &err);
    if (!doc) return std::nullopt;
    std::unique_ptr<yyjson_doc, void (*)(yyjson_doc*)> owner(doc, yyjson_doc_free);
    Json out;
    if (!from_yyjson(yyjson_doc_get_root(doc), out, 0)) return std::nullopt;
    return out;
}

std::string Json::number(double v) {
    if (!std::isfinite(v)) return "0";
    MutDoc doc;
    std::string out = write(doc.d, to_yyjson(doc.d, Json(v)));
    // A real too large for the integer path that is still integral (1e20): yyjson adds ".0".
    if (out.size() > 2 && out.compare(out.size() - 2, 2, ".0") == 0) out.resize(out.size() - 2);
    return out;
}

std::string Json::quote(std::string_view s) {
    MutDoc doc;
    return write(doc.d, yyjson_mut_strn(doc.d, s.data(), s.size()));
}

double Json::parseDouble(const char* s) {
    if (!s) return 0;
    // JSON number syntax has no leading '+' and no leading '.': CSS text has both.
    std::string t;
    if (*s == '+') s++;
    bool neg = *s == '-';
    if (neg) s++;
    if (*s == '.') t = "0";
    t += s;
    yyjson_val v;
    if (!yyjson_read_number(t.c_str(), &v, 0, nullptr, nullptr)) return 0;
    double d = yyjson_get_num(&v);
    return neg ? -d : d;
}

} // namespace twidget
