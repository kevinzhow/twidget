// SPEC S9.5. Parsing: litehtml's CSS Syntax 3 tokenizer and parser (the renderer's own). Checking: profile.h.
// Writing: our serializer below (CSSOM §2.1 for identifiers and strings, CSS Syntax 3 §9 "Serialization" for the
// comments that keep adjacent tokens apart), because litehtml's get_repr() gives back the original text.
#include "css_sanitizer.h"

#include "profile.h"
#include "urls.h"

#include <litehtml/css_parser.h>
#include <litehtml/css_selector.h>
#include <litehtml/css_tokenizer.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace twcard {

using litehtml::css_token;
using litehtml::css_token_vector;
namespace lh = litehtml;

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return out;
}

size_t utf16_units(std::string_view s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) == 0x80) continue;
        n += c >= 0xF0 ? 2 : 1;
    }
    return n;
}

bool is_ws(const css_token& t) { return t.type == lh::WHITESPACE; }
bool is_delim(const css_token& t, char c) { return t.type == c; }
bool is_ident(const css_token& t, std::string_view lower_name) { return t.type == lh::IDENT && lower(t.name()) == lower_name; }

/// Tokens without whitespace (top level only).
css_token_vector trimmed(const css_token_vector& v) {
    css_token_vector out;
    for (const css_token& t : v)
        if (!is_ws(t)) out.push_back(t);
    return out;
}

/// Splits at top-level commas, whitespace around each part trimmed.
std::vector<css_token_vector> split_commas(const css_token_vector& v) {
    std::vector<css_token_vector> out(1);
    for (const css_token& t : v) {
        if (t.type == lh::COMMA) out.emplace_back();
        else out.back().push_back(t);
    }
    for (auto& part : out) {
        while (!part.empty() && is_ws(part.front())) part.erase(part.begin());
        while (!part.empty() && is_ws(part.back())) part.pop_back();
    }
    return out;
}

css_token_vector parse_tokens(std::string_view text) { return lh::normalize(std::string(text), lh::f_componentize); }

bool css_wide_keyword(std::string_view l) { return l == "initial" || l == "inherit" || l == "unset" || l == "revert" || l == "revert-layer"; }

// ───────────────────────────── serializer ─────────────────────────────

bool ident_code_point(unsigned char c) { return c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }

void escape_code_point(std::string& out, unsigned cp) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "\\%x ", cp);
    out += buf;
}

/// The serialized form of a hash's name: like an identifier, but it may start with a digit (CSSOM "serialize a name").
std::string serialize_name(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == 0) out += "\xEF\xBF\xBD";
        else if (c < 0x20 || c == 0x7F) escape_code_point(out, c);
        else if (ident_code_point(c)) out += char(c);
        else {
            out += '\\';
            out += char(c);
        }
    }
    return out;
}

/// The leading number of a numeric token's original text, when it is well formed ([+-]digits[.digits][e[+-]digits]).
std::string number_text(const css_token& t) {
    const std::string& r = t.repr;
    size_t i = 0;
    if (i < r.size() && (r[i] == '+' || r[i] == '-')) i++;
    size_t d0 = i;
    while (i < r.size() && r[i] >= '0' && r[i] <= '9') i++;
    if (i < r.size() && r[i] == '.' && i + 1 < r.size() && r[i + 1] >= '0' && r[i + 1] <= '9') {
        i++;
        while (i < r.size() && r[i] >= '0' && r[i] <= '9') i++;
    }
    bool digits = i > d0;
    if (digits && i < r.size() && (r[i] == 'e' || r[i] == 'E')) {
        size_t j = i + 1;
        if (j < r.size() && (r[j] == '+' || r[j] == '-')) j++;
        size_t e0 = j;
        while (j < r.size() && r[j] >= '0' && r[j] <= '9') j++;
        if (j > e0) i = j;
    }
    if (digits) return r.substr(0, i);
    char buf[32];
    auto res = std::to_chars(buf, buf + sizeof buf, t.n.number);
    return std::string(buf, res.ptr);
}

/// The first and last "token kinds" a component value writes, for CSS Syntax 3 §9's comment table.
enum class Edge { None, Ident, Function, Url, Number, Percentage, Dimension, AtKeyword, Hash, Delim, OpenParen, Other };
struct Written {
    Edge first = Edge::None, last = Edge::None;
    char firstDelim = 0, lastDelim = 0;
};

bool needs_comment(Edge a, char ad, Edge b, char bd) {
    auto any = [&](std::initializer_list<Edge> list) { return std::find(list.begin(), list.end(), b) != list.end(); };
    const bool bMinus = b == Edge::Delim && bd == '-';
    switch (a) {
    case Edge::Ident: return any({Edge::Ident, Edge::Function, Edge::Url, Edge::Number, Edge::Percentage, Edge::Dimension, Edge::OpenParen}) || bMinus;
    case Edge::AtKeyword:
    case Edge::Hash:
    case Edge::Dimension: return any({Edge::Ident, Edge::Function, Edge::Url, Edge::Number, Edge::Percentage, Edge::Dimension}) || bMinus;
    case Edge::Number: return any({Edge::Ident, Edge::Function, Edge::Url, Edge::Number, Edge::Percentage, Edge::Dimension}) || (b == Edge::Delim && bd == '%');
    case Edge::Delim:
        if (ad == '#') return any({Edge::Ident, Edge::Function, Edge::Url, Edge::Number, Edge::Percentage, Edge::Dimension}) || bMinus;
        if (ad == '-') return any({Edge::Ident, Edge::Function, Edge::Url, Edge::Number, Edge::Percentage, Edge::Dimension});
        if (ad == '@') return any({Edge::Ident, Edge::Function, Edge::Url}) || bMinus;
        if (ad == '.' || ad == '+') return any({Edge::Number, Edge::Percentage, Edge::Dimension});
        if (ad == '/') return b == Edge::Delim && bd == '*';
        return false;
    default: return false;
    }
}

void write_tokens(const css_token_vector& v, std::string& out, Written* w = nullptr);

Written write_token(const css_token& t, std::string& out) {
    Written w;
    auto both = [&](Edge e, char d = 0) {
        w.first = w.last = e;
        w.firstDelim = w.lastDelim = d;
    };
    switch (t.type) {
    case lh::WHITESPACE: out += ' '; both(Edge::None); break;
    case lh::IDENT: out += css_serialize_identifier(t.name()); both(Edge::Ident); break;
    case lh::AT_KEYWORD: out += '@'; out += css_serialize_identifier(t.name()); both(Edge::AtKeyword); break;
    case lh::HASH: out += '#'; out += serialize_name(t.name()); both(Edge::Hash); break;
    case lh::STRING: out += css_serialize_string(t.str()); both(Edge::Other); break;
    case lh::URL: out += "url(" + css_serialize_string(t.str()) + ")"; both(Edge::Url); w.last = Edge::Other; break;
    case lh::NUMBER: out += number_text(t); both(Edge::Number); break;
    case lh::PERCENTAGE: out += number_text(t) + "%"; both(Edge::Percentage); w.last = Edge::Other; break;
    case lh::DIMENSION: {
        const std::string& u = t.unit();
        out += number_text(t);
        // A unit that reads as an exponent (`e3`, `e-1`): its `e` escaped, so the number stays the number.
        size_t d = u.size() > 1 && (u[1] == '+' || u[1] == '-') ? 2 : 1;
        if (!u.empty() && (u[0] == 'e' || u[0] == 'E') && d < u.size() && u[d] >= '0' && u[d] <= '9') {
            escape_code_point(out, (unsigned char)u[0]);
            out += css_serialize_identifier(std::string("a") + u.substr(1)).substr(1);
        } else {
            out += css_serialize_identifier(u);
        }
        both(Edge::Dimension);
        break;
    }
    case lh::CV_FUNCTION: {
        out += css_serialize_identifier(t.name()) + "(";
        write_tokens(t.value, out);
        out += ")";
        w.first = Edge::Function;
        w.last = Edge::Other;
        break;
    }
    case lh::ROUND_BLOCK:
    case lh::SQUARE_BLOCK:
    case lh::CURLY_BLOCK: {
        char open = t.type == lh::ROUND_BLOCK ? '(' : t.type == lh::SQUARE_BLOCK ? '[' : '{';
        char close = open == '(' ? ')' : open == '[' ? ']' : '}';
        out += open;
        write_tokens(t.value, out);
        out += close;
        w.first = open == '(' ? Edge::OpenParen : Edge::Other;
        w.last = Edge::Other;
        break;
    }
    case lh::COLON: out += ':'; both(Edge::Other); break;
    case lh::SEMICOLON: out += ';'; both(Edge::Other); break;
    case lh::COMMA: out += ','; both(Edge::Other); break;
    default:
        if (t.type > 0 && t.type < 0x80) {
            out += char(t.type);
            both(Edge::Delim, char(t.type));
        } else {
            both(Edge::Other);
        }
    }
    return w;
}

void write_tokens(const css_token_vector& v, std::string& out, Written* all) {
    Written prev;
    bool first = true;
    for (const css_token& t : v) {
        std::string piece;
        Written w = write_token(t, piece);
        // A number written with a leading '+' cannot continue an identifier or a dimension ('+' is no ident code
        // point), so it needs no comment after one.
        const bool plusNumber = !piece.empty() && piece[0] == '+' &&
                                (w.first == Edge::Number || w.first == Edge::Percentage || w.first == Edge::Dimension) &&
                                (prev.last == Edge::Ident || prev.last == Edge::Dimension || prev.last == Edge::Hash || prev.last == Edge::AtKeyword);
        if (!first && !plusNumber && needs_comment(prev.last, prev.lastDelim, w.first, w.firstDelim)) out += "/**/";
        out += piece;
        if (first && all) {
            all->first = w.first;
            all->firstDelim = w.firstDelim;
        }
        prev = w;
        first = false;
    }
    if (all) {
        all->last = prev.last;
        all->lastDelim = prev.lastDelim;
    }
}

std::string serialize(const css_token_vector& v) {
    std::string out;
    write_tokens(v, out);
    // Leading/trailing whitespace carries nothing.
    size_t b = out.find_first_not_of(' '), e = out.find_last_not_of(' ');
    return b == std::string::npos ? std::string() : out.substr(b, e - b + 1);
}

} // namespace

std::string css_serialize_identifier(std::string_view s) {
    std::string out;
    if (s == "-") return "\\-";
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0) out += "\xEF\xBF\xBD";
        else if (c < 0x20 || c == 0x7F) escape_code_point(out, c);
        else if (i == 0 && c >= '0' && c <= '9') escape_code_point(out, c);
        else if (i == 1 && c >= '0' && c <= '9' && s[0] == '-') escape_code_point(out, c);
        else if (ident_code_point(c)) out += char(c);
        else {
            out += '\\';
            out += char(c);
        }
    }
    return out;
}

std::string css_serialize_string(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == 0) out += "\xEF\xBF\xBD";
        else if (c < 0x20 || c == 0x7F) escape_code_point(out, c);
        else if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else out += char(c);
    }
    return out + "\"";
}

// ───────────────────────────── checking ─────────────────────────────

/// One sanitising pass: checks and rewrites component values of one declaration.
struct CssWork {
    const CssSanitizer& s;
    CssSanitizer::Counts* counts = nullptr;
    bool changed = false;     // a rewrite that changes meaning (supports() answers false then)
    std::string reason;       // why the declaration is dropped
    std::string reasonKind;   // its diagnostic kind

    static constexpr int kMaxDepth = 64;  // SPEC S9.5 and S9 conformance: bound nested component walks.

    bool fail(std::string_view kind, std::string why) {
        if (reason.empty()) {
            reasonKind = std::string(kind);
            reason = std::move(why);
        }
        return false;
    }

    static bool contains_function(const css_token_vector& v, std::initializer_list<std::string_view> names) {
        for (const css_token& t : v) {
            if (t.type == lh::CV_FUNCTION) {
                std::string n = lower(t.name());
                for (auto x : names)
                    if (n == x) return true;
            }
            if (t.is_component_value() && contains_function(t.value, names)) return true;
        }
        return false;
    }

    bool url_token(css_token& t, std::string_view raw) {
        urls::Verdict v = urls::check(raw);
        if (v.kind == urls::Kind::Blocked) return fail("url-blocked", std::string(raw.substr(0, 64)));
        t = css_token(lh::URL, v.url);
        return true;
    }

    /// The generic checks of every value (functions, units, URLs, env(), attr()), in place.
    bool values(css_token_vector& v, const profile::PropertyRule& rule, int depth) {
        if (depth > kMaxDepth) return fail("css-dropped", "nesting");
        for (size_t i = 0; i < v.size(); i++) {
            css_token& t = v[i];
            switch (t.type) {
            case lh::WHITESPACE:
            case lh::COMMA:
            case lh::IDENT:
            case lh::HASH:
            case lh::NUMBER:
            case lh::PERCENTAGE: break;
            case lh::STRING:
                if (counts) counts->stringUnits += utf16_units(t.str());
                break;
            case lh::DIMENSION:
                if (!profile::unit_allowed(lower(t.unit()))) return fail("profile", "unit " + t.unit());
                break;
            case lh::URL:
                if (!url_token(t, t.str())) return false;
                break;
            case lh::CV_FUNCTION: {
                const std::string name = lower(t.name());
                if (name == "url") {
                    css_token_vector args = trimmed(t.value);
                    if (args.size() != 1 || args[0].type != lh::STRING) return fail("url-blocked", "url() that cannot be checked");
                    if (!url_token(t, args[0].str())) return false;
                    break;
                }
                if (name == "env") {
                    if (!env(v, i, rule, depth)) return false;
                    i--;  // look at what replaced it
                    continue;
                }
                if (name == "attr") {
                    if (rule.value != profile::Value::Content) return fail("profile", "attr() outside content");
                    css_token_vector args = trimmed(t.value);
                    if (args.empty() || args[0].type != lh::IDENT || lower(args[0].name()).rfind("data-", 0) != 0)
                        return fail("profile", "attr() of no data-* attribute");
                    for (size_t k = 1; k < args.size(); k++)
                        if (args[k].type != lh::COMMA && args[k].type != lh::STRING) return fail("profile", "attr() fallback");
                    break;
                }
                if (name == "var") {
                    css_token_vector args = trimmed(t.value);
                    if (args.empty() || args[0].type != lh::IDENT || args[0].name().rfind("--", 0) != 0) return fail("css-dropped", "var()");
                    if (!values(t.value, rule, depth + 1)) return false;
                    break;
                }
                if (!profile::function_allowed(name, rule)) return fail("profile", name + "()");
                // A function that may take a URL needs it checkable: no var()/env()/attr() inside (S9.4).
                if (!values(t.value, rule, depth + 1)) return false;
                break;
            }
            case lh::ROUND_BLOCK:
            case lh::SQUARE_BLOCK:
                if (!values(t.value, rule, depth + 1)) return false;
                break;
            default:
                if (t.type > 0 && t.type < 0x80) {
                    char c = char(t.type);
                    if (c == '/' || c == '*' || c == '+' || c == '-' || c == '.' || c == '%') break;
                }
                return fail("css-dropped", "unexpected token");
            }
        }
        return true;
    }

    /// env(name[, fallback]) at v[i]: the safe-area insets become our variables, anything else its fallback (S9.5).
    bool env(css_token_vector& v, size_t i, const profile::PropertyRule& rule, int depth) {
        css_token_vector args = v[i].value;
        size_t k = 0;
        while (k < args.size() && is_ws(args[k])) k++;
        if (k >= args.size() || args[k].type != lh::IDENT) return fail("css-dropped", "env()");
        std::string name = lower(args[k].name());
        size_t comma = k + 1;
        while (comma < args.size() && args[comma].type != lh::COMMA) comma++;
        css_token_vector fallback(args.begin() + std::min(comma + 1, args.size()), args.end());
        static const char* insets[] = {"top", "right", "bottom", "left"};
        for (const char* side : insets) {
            if (name != std::string("safe-area-inset-") + side) continue;
            css_token var(lh::CV_FUNCTION, "var");
            var.value.push_back(css_token(lh::IDENT, std::string("--tg-content-safe-area-inset-") + side));
            if (comma < args.size()) {
                var.value.push_back(css_token(lh::COMMA, ","));
                for (auto& f : fallback) var.value.push_back(f);
            }
            if (!values(var.value, rule, depth + 1)) return false;
            v[i] = std::move(var);
            return true;
        }
        if (comma >= args.size()) return fail("profile", "env(" + name + ")");
        changed = true;
        v.erase(v.begin() + long(i));
        v.insert(v.begin() + long(i), fallback.begin(), fallback.end());
        return true;
    }

    // ── property-specific rules ──

    bool keywords(css_token_vector& v, const profile::PropertyRule& rule) {
        bool any = false;
        for (css_token& t : v) {
            if (is_ws(t)) continue;
            if (t.type != lh::IDENT) return fail("profile", std::string(rule.name) + " value");
            std::string l = lower(t.name());
            std::string_view list = rule.keywords;
            bool found = false;
            size_t at = 0;
            while (at <= list.size()) {
                size_t sp = list.find(' ', at);
                std::string_view item = list.substr(at, sp == std::string_view::npos ? std::string_view::npos : sp - at);
                size_t eq = item.find('=');
                if (item.substr(0, eq) == l) {
                    found = true;
                    if (eq != std::string_view::npos) {
                        t = css_token(lh::IDENT, std::string(item.substr(eq + 1)));
                        changed = true;
                    }
                    break;
                }
                if (sp == std::string_view::npos) break;
                at = sp + 1;
            }
            if (!found) return fail("profile", std::string(rule.name) + ": " + l);
            any = true;
        }
        return any || fail("css-dropped", "empty");
    }

    bool single_keyword(const css_token_vector& v, std::initializer_list<std::string_view> allowed, std::string_view prop) {
        css_token_vector t = trimmed(v);
        if (t.size() != 1 || t[0].type != lh::IDENT) return fail("profile", std::string(prop) + " value");
        std::string l = lower(t[0].name());
        for (auto a : allowed)
            if (l == a) return true;
        return fail("profile", std::string(prop) + ": " + l);
    }

    /// A comma-separated family list (`font-family`, the tail of `font`): generic families only.
    bool families(css_token_vector& v, bool shorthandTail) {
        std::vector<css_token_vector> parts = split_commas(v);
        css_token_vector out;
        for (auto& part : parts) {
            css_token_vector p = trimmed(part);
            if (p.size() == 1 && p[0].type == lh::IDENT) {
                std::string l = lower(p[0].name());
                if (l == "-apple-system") {
                    l = "system-ui";
                    changed = true;
                }
                if (profile::generic_family(l)) {
                    if (!out.empty()) out.push_back(css_token(lh::COMMA, ","));
                    out.push_back(css_token(lh::IDENT, l));
                    continue;
                }
            }
            changed = true;  // "other names removed"
        }
        if (out.empty()) {
            if (!shorthandTail) return fail("profile", "font-family: no generic family");
            out.push_back(css_token(lh::IDENT, "system-ui"));  // decision: a shorthand keeps its size, on system-ui
        }
        v = std::move(out);
        return true;
    }

    bool font(css_token_vector& v) {
        css_token_vector t = trimmed(v);
        if (t.size() == 1 && t[0].type == lh::IDENT) return true;  // a system font keyword
        // The family list starts after the size (and an optional `/ line-height`).
        static const std::initializer_list<std::string_view> sizes = {"xx-small", "x-small", "small", "medium", "large",
                                                                      "x-large", "xx-large", "xxx-large", "larger", "smaller"};
        size_t i = 0, sizeAt = std::string::npos;
        for (; i < v.size(); i++) {
            const css_token& x = v[i];
            if (x.type == lh::COMMA) break;
            bool isSize = x.type == lh::DIMENSION || x.type == lh::PERCENTAGE ||
                          (x.type == lh::CV_FUNCTION && lower(x.name()) != "var") ||
                          (x.type == lh::IDENT && std::find(sizes.begin(), sizes.end(), lower(x.name())) != sizes.end());
            if (isSize) sizeAt = i;
        }
        if (sizeAt == std::string::npos) return fail("css-dropped", "font without a size");
        size_t start = sizeAt + 1;
        // skip whitespace, `/`, line-height
        size_t j = start;
        while (j < v.size() && is_ws(v[j])) j++;
        if (j < v.size() && is_delim(v[j], '/')) {
            j++;
            while (j < v.size() && is_ws(v[j])) j++;
            if (j < v.size()) j++;  // the line height
            start = j;
        }
        css_token_vector head(v.begin(), v.begin() + long(start)), tail(v.begin() + long(start), v.end());
        if (!families(tail, true)) return false;
        head.push_back(css_token(lh::WHITESPACE, " "));
        head.insert(head.end(), tail.begin(), tail.end());
        v = std::move(head);
        return true;
    }

    bool only_functions(const css_token_vector& v, std::string_view prop) {
        css_token_vector t = trimmed(v);
        if (t.size() == 1 && t[0].type == lh::IDENT && lower(t[0].name()) == "none") return true;
        for (const css_token& x : t)
            if (x.type != lh::CV_FUNCTION) return fail("profile", std::string(prop) + " value");
        return !t.empty() || fail("css-dropped", "empty");
    }

    bool content(const css_token_vector& v) {
        for (const css_token& x : trimmed(v)) {
            if (x.type == lh::STRING || x.type == lh::CV_FUNCTION || is_delim(x, '/')) continue;
            if (x.type == lh::IDENT) {
                std::string l = lower(x.name());
                if (l == "none" || l == "normal" || l == "open-quote" || l == "close-quote" || l == "no-open-quote" || l == "no-close-quote")
                    continue;
            }
            return fail("profile", "content value");
        }
        return true;
    }

    /// The whole check of one declaration; `v` is rewritten in place.
    bool declaration(const std::string& name, css_token_vector& v) {
        static const profile::PropertyRule kCustom{"--", profile::Value::Any};
        const profile::PropertyRule* rule = name.rfind("--", 0) == 0 ? &kCustom : profile::property(name);
        if (!rule) return fail("profile", "property " + name);
        if (trimmed(v).empty()) return fail("css-dropped", name + ": empty");
        const bool custom = rule == &kCustom;
        // Custom properties may feed any property: every function of the profile is in for them.
        static const profile::PropertyRule kAnyFn{"--", profile::Value::Content};
        if (custom) {
            if (!values(v, kAnyFn, 0)) {
                // A custom property may hold transform/filter functions too.
                reason.clear();
                css_token_vector copy = v;
                static const profile::PropertyRule kT{"--", profile::Value::Transform}, kF{"--", profile::Value::Filter};
                if (!values(copy, kT, 0)) {
                    reason.clear();
                    copy = v;
                    if (!values(copy, kF, 0)) return false;
                }
                v = std::move(copy);
            }
            return true;
        }
        if (!values(v, *rule, 0)) return false;
        css_token_vector t = trimmed(v);
        if (t.size() == 1 && t[0].type == lh::IDENT && css_wide_keyword(lower(t[0].name()))) return true;
        // A var() anywhere: the property-specific checks cannot be made before substitution; URLs and functions were.
        if (contains_function(v, {"var"})) return true;
        // Border styles: "borders (solid, dashed, dotted, double)".
        const bool borderStyle = name == "border" || name == "outline" || name == "outline-style" || name == "border-style" ||
                                 name == "border-top" || name == "border-right" || name == "border-bottom" || name == "border-left" ||
                                 (name.rfind("border-", 0) == 0 && name.size() > 6 && name.compare(name.size() - 6, 6, "-style") == 0);
        if (borderStyle) {
            for (const css_token& x : t)
                if (x.type == lh::IDENT) {
                    std::string l = lower(x.name());
                    if (l == "groove" || l == "ridge" || l == "inset" || l == "outset") return fail("profile", name + ": " + l);
                }
        }
        switch (rule->value) {
        case profile::Value::Any: break;
        case profile::Value::Keywords: return keywords(v, *rule);
        case profile::Value::Display:
            return single_keyword(v, {"none", "block", "inline", "inline-block", "flex", "inline-flex", "grid", "inline-grid", "table",
                                      "inline-table", "table-row", "table-cell", "table-row-group", "table-header-group",
                                      "table-footer-group", "table-column", "table-column-group", "table-caption"},
                                  name);
        case profile::Value::TouchAction: return single_keyword(v, {"auto", "pan-y", "none"}, name);
        case profile::Value::FontFamily: return families(v, false);
        case profile::Value::Font: return font(v);
        case profile::Value::Transform:
        case profile::Value::Filter:
        case profile::Value::BackdropFilter: return only_functions(v, name);
        case profile::Value::Content: return content(v);
        }
        // The individual transform properties: 2D only (guide: out "3D transforms").
        if (name == "rotate" && t.size() != 1) return fail("profile", "rotate: 3D");
        if ((name == "scale" || name == "translate") && t.size() > 2) return fail("profile", name + ": 3D");
        return true;
    }
};

namespace {

/// A parsed declaration list's declarations, sanitised, as `name: value[ !important]`; drops reported.
template <class Emit> void declarations(const CssSanitizer& s, CssSanitizer::Counts* counts, const lh::raw_declaration::vector& decls,
                                        bool keyframe, Emit emit) {
    for (const lh::raw_declaration& d : decls) {
        std::string name = d.name.rfind("--", 0) == 0 ? d.name : lower(d.name);
        if (keyframe) {
            // In @keyframes: !important declarations are ignored, and so are animation properties except
            // animation-timing-function (CSS Animations 1, §3).
            if (d.important) continue;
            if (name.rfind("animation", 0) == 0 && name != "animation-timing-function") continue;
        }
        css_token_vector v = d.value;
        CssWork w{s, counts};
        if (!w.declaration(name, v)) {
            s.diag(w.reasonKind, w.reason);
            continue;
        }
        std::string value = serialize(v);
        if (value.empty()) continue;
        emit(css_serialize_identifier(name), value, d.important);
    }
}

void parse_block(const css_token& block, lh::raw_declaration::vector& decls, lh::raw_rule::vector& rules) {
    lh::css_parser p(block.value);
    p.consume_style_block_contents(decls, rules);
}

} // namespace

std::optional<std::string> CssSanitizer::value(std::string_view property, std::string_view text, Counts* counts) const {
    std::string name = property.substr(0, 2) == "--" ? std::string(property) : lower(property);
    css_token_vector v = parse_tokens(text);
    // A trailing `!important` belongs to the declaration, not to the value (callers pass it separately).
    CssWork w{*this, counts};
    if (!w.declaration(name, v)) {
        diag(w.reasonKind, w.reason);
        return std::nullopt;
    }
    std::string out = serialize(v);
    if (out.empty()) return std::nullopt;
    return out;
}

bool CssSanitizer::supportsDeclaration(std::string_view property, std::string_view text) const {
    std::string name = property.substr(0, 2) == "--" ? std::string(property) : lower(property);
    css_token_vector v = parse_tokens(text);
    CssWork w{*this, nullptr};
    return w.declaration(name, v) && !w.changed;
}

std::string CssSanitizer::declarationList(std::string_view css, Counts* counts) const {
    css_token_vector tokens = parse_tokens(css);
    lh::css_parser p(tokens);
    lh::raw_declaration::vector decls;
    lh::raw_rule::vector rules;
    p.consume_style_block_contents(decls, rules);
    if (!rules.empty()) diag("profile", "nested rules in a style attribute");
    std::string out;
    declarations(*this, counts, decls, false, [&](const std::string& n, const std::string& v, bool important) {
        if (!out.empty()) out += "; ";
        out += n + ": " + v + (important ? " !important" : "");
    });
    return out;
}

// ── selectors ──

namespace {

struct SelectorCheck {
    const CssSanitizer& s;
    std::string why;

    bool list(const css_token_vector& v, int depth) {
        for (const css_token_vector& part : split_commas(v))
            if (part.empty() || !complex(part, depth)) return false;
        return true;
    }

    bool nth_args(const css_token_vector& v, int depth) {
        for (size_t i = 0; i < v.size(); i++) {
            const css_token& t = v[i];
            if (t.type == lh::IDENT && lower(t.name()) == "of") {
                css_token_vector rest(v.begin() + long(i) + 1, v.end());
                return list(rest, depth + 1);
            }
            if (t.type == lh::WHITESPACE || t.type == lh::NUMBER || t.type == lh::DIMENSION || t.type == lh::IDENT || is_delim(t, '+') ||
                is_delim(t, '-'))
                continue;
            return false;
        }
        return true;
    }

    bool complex(const css_token_vector& v, int depth) {
        if (depth > 16) return false;
        for (size_t i = 0; i < v.size(); i++) {
            const css_token& t = v[i];
            if (t.type == lh::COLON) {
                if (i + 1 >= v.size()) return false;
                const css_token& n = v[i + 1];
                if (n.type == lh::COLON) {
                    if (i + 2 >= v.size() || v[i + 2].type != lh::IDENT || !profile::pseudo_element_allowed(lower(v[i + 2].name()))) {
                        why = "pseudo-element";
                        return false;
                    }
                    i += 2;
                    continue;
                }
                if (n.type == lh::IDENT) {
                    if (!profile::pseudo_class_allowed(lower(n.name()), false)) {
                        why = ":" + lower(n.name());
                        return false;
                    }
                    i++;
                    continue;
                }
                if (n.type == lh::CV_FUNCTION) {
                    std::string f = lower(n.name());
                    if (!profile::pseudo_class_allowed(f, true)) {
                        why = ":" + f + "()";
                        return false;
                    }
                    bool ok = f.rfind("nth-", 0) == 0 ? nth_args(n.value, depth) : list(n.value, depth + 1);
                    if (!ok) return false;
                    i++;
                    continue;
                }
                return false;
            }
            if (t.type == lh::IDENT || t.type == lh::HASH || t.type == lh::WHITESPACE || t.type == lh::SQUARE_BLOCK) continue;
            if (is_delim(t, '.') || is_delim(t, '*') || is_delim(t, '>') || is_delim(t, '+') || is_delim(t, '~')) continue;
            why = "selector syntax";  // `&` nesting, `|` namespaces, anything else
            return false;
        }
        return true;
    }
};

} // namespace

// ── media queries ──

namespace {

bool media_value_ok(const css_token_vector& v) {
    for (const css_token& t : v) {
        if (t.type == lh::WHITESPACE || t.type == lh::NUMBER || t.type == lh::IDENT || is_delim(t, '/')) continue;
        if (t.type == lh::DIMENSION && profile::unit_allowed(lower(t.unit()))) continue;
        return false;
    }
    return true;
}

bool media_condition(const css_token_vector& v, int depth);

bool media_in_parens(const css_token& block, int depth) {
    if (block.type != lh::ROUND_BLOCK || depth > 16) return false;
    css_token_vector t = trimmed(block.value);
    if (t.empty()) return false;
    if (t[0].type == lh::ROUND_BLOCK || is_ident(t[0], "not")) return media_condition(block.value, depth + 1);
    // `(feature: value)` / `(feature)`
    if (t[0].type == lh::IDENT && (t.size() == 1 || t[1].type == lh::COLON)) {
        if (!profile::media_feature_allowed(lower(t[0].name()))) return false;
        css_token_vector value(t.begin() + std::min<long>(2, long(t.size())), t.end());
        return t.size() == 1 || (!value.empty() && media_value_ok(value));
    }
    // range syntax: (width >= 300px), (300px < width < 600px)
    bool feature = false;
    for (const css_token& x : t) {
        if (x.type == lh::IDENT) {
            if (!profile::media_feature_allowed(lower(x.name())) || lower(x.name()).rfind("min-", 0) == 0 || lower(x.name()).rfind("max-", 0) == 0)
                return false;
            feature = true;
            continue;
        }
        if (is_delim(x, '<') || is_delim(x, '>') || is_delim(x, '=') || x.type == lh::NUMBER) continue;
        if (x.type == lh::DIMENSION && profile::unit_allowed(lower(x.unit()))) continue;
        return false;
    }
    return feature;
}

bool media_condition(const css_token_vector& v, int depth) {
    css_token_vector t = trimmed(v);
    if (t.empty()) return false;
    for (const css_token& x : t) {
        if (x.type == lh::ROUND_BLOCK) {
            if (!media_in_parens(x, depth)) return false;
        } else if (!(is_ident(x, "and") || is_ident(x, "or") || is_ident(x, "not"))) {
            return false;
        }
    }
    return true;
}

bool media_query(const css_token_vector& q) {
    css_token_vector t = trimmed(q);
    if (t.empty()) return false;
    size_t i = 0;
    if (t[0].type == lh::ROUND_BLOCK || (is_ident(t[0], "not") && t.size() > 1 && t[1].type == lh::ROUND_BLOCK)) return media_condition(t, 0);
    if (is_ident(t[0], "not") || is_ident(t[0], "only")) i++;
    if (i >= t.size() || t[i].type != lh::IDENT || !profile::media_type_allowed(lower(t[i].name()))) return false;
    i++;
    if (i == t.size()) return true;
    if (!is_ident(t[i], "and")) return false;
    css_token_vector rest(t.begin() + long(i) + 1, t.end());
    for (const css_token& x : rest)
        if (x.type != lh::ROUND_BLOCK && !is_ident(x, "and")) return false;  // no `or` after a media type
    return media_condition(rest, 0);
}

// SPEC S10.2: resolve theme-dependent media features before dispatching layout queries.
void resolve_media_preferences(css_token_vector& tokens, bool dark) {
    for (auto& token : tokens) {
        if (token.type != lh::ROUND_BLOCK) continue;
        const auto values = trimmed(token.value);
        if (values.empty()) continue;
        bool matches;
        if (is_ident(values.front(), "prefers-color-scheme")) {
            matches = values.size() == 1 ||
                (values.size() == 3 && is_ident(values.back(), dark ? "dark" : "light"));
        } else if (is_ident(values.front(), "prefers-reduced-motion")) {
            matches = values.size() == 3 && is_ident(values.back(), "no-preference");
        } else {
            resolve_media_preferences(token.value, dark);
            continue;
        }
        token = std::move(parse_tokens(matches ? "(min-width: 0px)" : "(width < 0px)").front());
    }
}

/// The queries of a list that can match in profile 1, serialised; nullopt when none.
std::optional<std::string> media_list(const css_token_vector& prelude, std::optional<bool> dark = std::nullopt) {
    std::string out;
    for (css_token_vector& q : split_commas(prelude)) {
        if (!media_query(q)) continue;
        if (dark) resolve_media_preferences(q, *dark);
        if (!out.empty()) out += ", ";
        out += serialize(q);
    }
    if (out.empty()) return std::nullopt;
    return out;
}

} // namespace

std::optional<std::string> CssSanitizer::mediaQueryList(std::string_view text, std::optional<bool> darkTheme) const {
    if (text.size() > 4096) return std::nullopt;
    auto r = media_list(parse_tokens(text), darkTheme);
    if (!r) diag("profile", "media query");
    return r;
}

// ── @supports ──

namespace {

bool supports_condition(const CssSanitizer& s, const css_token_vector& v, int depth);

bool supports_in_parens(const CssSanitizer& s, const css_token& t, int depth) {
    if (depth > 16) return false;
    if (t.type == lh::CV_FUNCTION) {
        if (lower(t.name()) != "selector") return false;  // font-tech(), font-format(), …: none in profile 1
        SelectorCheck c{s, {}};
        css_token_vector sel = trimmed(t.value);
        if (sel.empty() || !c.list(t.value, 0)) return false;
        litehtml::css_selector parsed;
        return parsed.parse(serialize(t.value), litehtml::no_quirks_mode);
    }
    if (t.type != lh::ROUND_BLOCK) return false;
    css_token_vector in = t.value;
    size_t k = 0;
    while (k < in.size() && is_ws(in[k])) k++;
    size_t c = k + 1;
    while (c < in.size() && is_ws(in[c])) c++;
    if (k < in.size() && in[k].type == lh::IDENT && c < in.size() && in[c].type == lh::COLON) {
        std::string name = in[k].name();
        css_token_vector value(in.begin() + long(c) + 1, in.end());
        // `!important` is allowed in a supports declaration and changes nothing.
        css_token_vector vt = trimmed(value);
        if (vt.size() >= 2 && vt[vt.size() - 2].type == lh::BANG && is_ident(vt.back(), "important")) {
            while (!value.empty() && value.back().type != lh::BANG) value.pop_back();
            if (!value.empty()) value.pop_back();
        }
        return s.supportsDeclaration(name, serialize(value));
    }
    return supports_condition(s, in, depth + 1);
}

bool supports_condition(const CssSanitizer& s, const css_token_vector& v, int depth) {
    css_token_vector t = trimmed(v);
    if (t.empty() || depth > 16) return false;
    if (is_ident(t[0], "not")) return t.size() == 2 && !supports_in_parens(s, t[1], depth + 1);
    bool result = supports_in_parens(s, t[0], depth + 1);
    std::string op;
    for (size_t i = 1; i < t.size(); i += 2) {
        if (i + 1 >= t.size() || t[i].type != lh::IDENT) return false;
        std::string o = lower(t[i].name());
        if ((o != "and" && o != "or") || (!op.empty() && o != op)) return false;  // mixing and/or needs parentheses
        op = o;
        bool next = supports_in_parens(s, t[i + 1], depth + 1);
        result = o == "and" ? result && next : result || next;
    }
    return result;
}

} // namespace

bool CssSanitizer::supportsCondition(std::string_view condition) const {
    if (condition.size() > 4096) return false;
    css_token_vector v = parse_tokens(condition);
    // CSS.supports(condition) also takes a bare declaration (CSSOM "supports(conditionText)").
    css_token_vector t = trimmed(v);
    if (!t.empty() && t[0].type == lh::IDENT && t.size() > 1 && t[1].type == lh::COLON) {
        css_token block(lh::ROUND_BLOCK);
        block.value = v;
        return supports_in_parens(*this, block, 0);
    }
    return supports_condition(*this, v, 0);
}

// ── easing ──

std::optional<std::string> CssSanitizer::easing(std::string_view text) const {
    if (text.size() > 4096) return std::nullopt;
    css_token_vector t = trimmed(parse_tokens(text));
    if (t.size() != 1) return std::nullopt;
    const css_token& x = t[0];
    if (x.type == lh::IDENT) {
        std::string l = lower(x.name());
        for (auto k : {"linear", "ease", "ease-in", "ease-out", "ease-in-out", "step-start", "step-end"})
            if (l == k) return l;
        return std::nullopt;
    }
    if (x.type != lh::CV_FUNCTION) return std::nullopt;
    std::string f = lower(x.name());
    std::vector<css_token_vector> args = split_commas(x.value);
    auto number = [](const css_token_vector& a, double& out) {
        if (a.size() != 1 || a[0].type != lh::NUMBER) return false;
        out = a[0].n.number;
        return std::isfinite(out);
    };
    if (f == "cubic-bezier") {
        double p[4];
        if (args.size() != 4) return std::nullopt;
        for (int i = 0; i < 4; i++)
            if (!number(args[size_t(i)], p[i])) return std::nullopt;
        if (p[0] < 0 || p[0] > 1 || p[2] < 0 || p[2] > 1) return std::nullopt;
        return serialize(t);
    }
    if (f == "steps") {
        double n;
        if (args.empty() || args.size() > 2 || !number(args[0], n) || n < 1 || n != std::floor(n)) return std::nullopt;
        if (args.size() == 2) {
            if (args[1].size() != 1 || args[1][0].type != lh::IDENT) return std::nullopt;
            std::string pos = lower(args[1][0].name());
            if (pos != "jump-start" && pos != "jump-end" && pos != "jump-none" && pos != "jump-both" && pos != "start" && pos != "end")
                return std::nullopt;
            if (pos == "jump-none" && n < 2) return std::nullopt;
        }
        return serialize(t);
    }
    if (f == "linear") {
        if (args.empty()) return std::nullopt;
        for (const auto& a : args) {
            css_token_vector p = trimmed(a);
            if (p.empty() || p.size() > 3 || p[0].type != lh::NUMBER) return std::nullopt;
            for (size_t i = 1; i < p.size(); i++)
                if (p[i].type != lh::PERCENTAGE) return std::nullopt;
        }
        return serialize(t);
    }
    return std::nullopt;
}

// ── stylesheets ──

namespace {

struct SheetWriter {
    const CssSanitizer& s;
    CssSanitizer::Counts& counts;
    std::string out;

    void rules(const lh::raw_rule::vector& list, int depth) {
        for (const auto& r : list) rule(*r, depth);
    }

    void rule(const lh::raw_rule& r, int depth) {
        if (depth > 16) {
            s.diag("css-dropped", "nesting");
            return;
        }
        if (r.type == lh::raw_rule::qualified) {
            style_rule(r);
            return;
        }
        const std::string name = lower(r.name);
        if (name == "media") {
            auto mq = media_list(r.prelude);
            if (!mq) {
                s.diag("profile", "@media " + serialize(r.prelude).substr(0, 64));
                return;
            }
            lh::css_parser p(r.block.value);
            auto inner = p.consume_list_of_rules(false);
            SheetWriter w{s, counts, {}};
            w.rules(inner, depth + 1);
            if (!w.out.empty()) out += "@media " + *mq + "{" + w.out + "}";
            return;
        }
        if (name == "supports") {
            if (!supports_condition(s, r.prelude, 0)) return;  // decided here (S9.5): the block goes
            lh::css_parser p(r.block.value);
            auto inner = p.consume_list_of_rules(false);
            rules(inner, depth + 1);  // kept: its rules stand on their own
            return;
        }
        if (name == "keyframes" || name == "-webkit-keyframes") {
            keyframes(r);
            return;
        }
        s.diag("profile", "@" + name);
    }

    void style_rule(const lh::raw_rule& r) {
        std::string selectors;
        for (const css_token_vector& part : split_commas(r.prelude)) {
            SelectorCheck c{s, {}};
            if (part.empty() || !c.complex(part, 0)) {
                s.diag("profile", "selector " + (c.why.empty() ? std::string("syntax") : c.why));
                continue;
            }
            std::string text = serialize(part);
            litehtml::css_selector parsed;
            if (!parsed.parse(text, litehtml::no_quirks_mode)) {
                s.diag("css-dropped", "selector " + text.substr(0, 64));
                continue;
            }
            if (!selectors.empty()) selectors += ",";
            selectors += text;
        }
        if (selectors.empty()) return;
        lh::raw_declaration::vector decls;
        lh::raw_rule::vector nested;
        parse_block(r.block, decls, nested);
        if (!nested.empty()) s.diag("profile", "CSS nesting");
        std::string body;
        declarations(s, &counts, decls, false, [&](const std::string& n, const std::string& v, bool important) {
            body += n + ":" + v + (important ? " !important" : "") + ";";
        });
        if (body.empty()) return;  // nothing of it is kept: no rule, and none counted
        counts.rules++;
        out += selectors + "{" + body + "}";
    }

    void keyframes(const lh::raw_rule& r) {
        css_token_vector p = trimmed(r.prelude);
        if (p.size() != 1 || (p[0].type != lh::IDENT && p[0].type != lh::STRING)) {
            s.diag("css-dropped", "@keyframes name");
            return;
        }
        std::string name = p[0].type == lh::IDENT ? css_serialize_identifier(p[0].name()) : css_serialize_string(p[0].str());
        if (p[0].type == lh::IDENT) {
            std::string l = lower(p[0].name());
            if (l == "none" || css_wide_keyword(l)) {
                s.diag("css-dropped", "@keyframes " + l);
                return;
            }
        }
        lh::css_parser parser(r.block.value);
        auto frames = parser.consume_list_of_rules(false);
        std::string body;
        for (const auto& f : frames) {
            if (f->type != lh::raw_rule::qualified) continue;
            std::string sel;
            bool ok = true;
            for (const css_token_vector& part : split_commas(f->prelude)) {
                css_token_vector t = trimmed(part);
                if (t.size() != 1) {
                    ok = false;
                    break;
                }
                std::string one;
                if (t[0].type == lh::PERCENTAGE && t[0].n.number >= 0 && t[0].n.number <= 100) one = number_text(t[0]) + "%";
                else if (is_ident(t[0], "from") || is_ident(t[0], "to")) one = lower(t[0].name());
                else {
                    ok = false;
                    break;
                }
                if (!sel.empty()) sel += ",";
                sel += one;
            }
            if (!ok || sel.empty()) {
                s.diag("css-dropped", "keyframe selector");
                continue;
            }
            lh::raw_declaration::vector decls;
            lh::raw_rule::vector nested;
            parse_block(f->block, decls, nested);
            std::string decl;
            declarations(s, &counts, decls, true, [&](const std::string& n, const std::string& v, bool) { decl += n + ":" + v + ";"; });
            body += sel + "{" + decl + "}";
        }
        counts.keyframes++;
        out += "@keyframes " + name + "{" + body + "}";
    }
};

} // namespace

std::string CssSanitizer::stylesheet(std::string_view css, Counts& counts) const {
    lh::raw_rule::vector rules = lh::css_parser::parse_stylesheet(std::string(css), true);
    SheetWriter w{*this, counts, {}};
    w.rules(rules, 0);
    return w.out;
}

} // namespace twcard
