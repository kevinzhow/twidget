// twidget::Json (base/include/twidget/json.h): the small JSON API TwidgetCore and the card host use, over yyjson.
// Requirements: SPEC S11.1 (strict RFC 8259, nesting limit, lone surrogates kept as WTF-8, shortest round-trip
// locale-independent numbers, duplicate keys: last wins).
#include "twidget/json.h"

#include <clocale>
#include <cmath>
#include <cstdio>
#include <string>

using twidget::Json;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) failures++;
}

void eq(const std::string& got, const std::string& want, const std::string& what) {
    check(got == want, what + (got == want ? "" : " (got " + got + ", want " + want + ")"));
}

void rejects(const std::string& text, const std::string& what) { check(!Json::parse(text), "rejects " + what); }

} // namespace

int main() {
    // Building and reading values.
    {
        Json o = Json::object();
        o.set("type", "event").set("n", 3).set("x", 1.5).set("ok", true).set("none", nullptr);
        Json a = Json::array();
        a.push(1).push("two").push(Json::object().set("k", "v"));
        o.set("list", std::move(a));
        eq(o.dump(), R"({"type":"event","n":3,"x":1.5,"ok":true,"none":null,"list":[1,"two",{"k":"v"}]})",
           "insertion order kept in dump (renderers match message prefixes)");
        check(o["type"].asString() == "event" && o["n"].asInt() == 3 && o["x"].asNumber() == 1.5 && o["ok"].asBool(), "getters");
        check(o["missing"].isNull() && o["missing"]["deeper"].isNull() && o["list"][size_t(9)].isNull(), "missing members read as null");
        check(o["n"].asString().empty() && o["type"].asNumber(7) == 7 && o["type"].asInt(-1) == -1 && o["type"].asBool(true),
              "wrong type gives the default");
        check(o.find("x") && !o.find("y") && o["list"].arr().size() == 3 && o.size() == 6, "find, arr, size");
        o.set("n", 4);
        check(o["n"].asInt() == 4 && o.size() == 6, "set replaces an existing member in place");
        int keys = 0;
        for (const auto& [k, v] : o.obj()) keys += !k.empty() && (v.isNull() || !v.isNull());
        check(keys == 6, "obj() iterates members");
    }
    // Strict parsing (RFC 8259).
    {
        auto j = Json::parse(R"( {"a": [1, -2.5e3, true, false, null, "s\né😀"]} )");
        check(j && (*j)["a"][size_t(1)].asNumber() == -2500, "parses a document");
        check(j && (*j)["a"][size_t(5)].asString() == "s\n\xc3\xa9\xf0\x9f\x98\x80", "escapes decode to UTF-8");
        rejects("{\"a\":1,}", "trailing comma");
        rejects("[1] 2", "trailing content");
        rejects("{'a':1}", "single quotes");
        rejects("[NaN]", "NaN");
        rejects("// c\n1", "comments");
        rejects("[01]", "leading zero");
        rejects("\"a\x01\"", "raw control character");
        rejects("", "empty input");
        check(Json::parse("1")->asInt() == 1 && Json::parse("\"x\"")->asString() == "x", "scalars at top level");
    }
    // Duplicate keys: last wins.
    {
        auto j = Json::parse(R"({"a":1,"b":2,"a":3})");
        check(j && (*j)["a"].asInt() == 3 && j->size() == 2, "duplicate keys: last wins");
    }
    // Nesting limit on hostile input.
    {
        std::string deep(Json::kMaxDepth, '['), close(Json::kMaxDepth, ']');
        check(bool(Json::parse(deep + close)), "nesting at the limit parses");
        rejects("[" + deep + close + "]", "nesting past the limit");
        std::string hostile(1000000, '[');
        rejects(hostile, "a million open brackets (no crash)");
        std::string objs;
        for (int i = 0; i < 100000; i++) objs += "{\"a\":";
        rejects(objs, "deep objects (no crash)");
    }
    // Lone surrogates: kept as WTF-8, written back as escapes.
    {
        auto j = Json::parse(R"(["\ud800", "a\udc00b", "😀", "\ude00\ud83d", "\\ud800"])");
        check(bool(j), "lone surrogate escapes parse");
        if (j) {
            eq((*j)[size_t(0)].asString(), "\xed\xa0\x80", "lone high surrogate as WTF-8");
            eq((*j)[size_t(1)].asString(), "a\xed\xb0\x80" "b", "lone low surrogate as WTF-8");
            eq((*j)[size_t(2)].asString(), "\xf0\x9f\x98\x80", "a pair stays one code point");
            eq((*j)[size_t(3)].asString(), "\xed\xb8\x80\xed\xa0\xbd", "reversed pair: two lone surrogates");
            eq((*j)[size_t(4)].asString(), "\\ud800", "an escaped backslash is not an escape");
            eq(j->dump(), R"(["\ud800","a\udc00b","😀","\ude00\ud83d","\\ud800"])", "dump writes lone surrogates as escapes");
        }
        Json s = Json::object().set("t", std::string("x\xed\xa0\x80y"));
        eq(s.dump(), R"({"t":"x\ud800y"})", "a WTF-8 string from C++ dumps as an escape");
        rejects("[\"\\ud800\\u12\"]", "a broken escape after a surrogate");
        Json bad = Json::object().set("t", std::string("a\xff" "b"));
        auto back = Json::parse(bad.dump());
        check(back && (*back)["t"].isString(), "invalid UTF-8 from C++ still dumps as valid JSON");
    }
    // Numbers: shortest round trip, integers without a fraction, locale independent.
    {
        std::setlocale(LC_NUMERIC, "de_DE.UTF-8");
        eq(Json::number(1), "1", "integral double");
        eq(Json::number(-0.0), "0", "negative zero");
        eq(Json::number(0.1), "0.1", "0.1 shortest");
        eq(Json::number(1.5), "1.5", "1.5 with a dot under any locale");
        eq(Json::number(1e21), "1e21", "large");
        eq(Json::number(123456789012.0), "123456789012", "large integral");
        eq(Json::number(5e-7), "5e-7", "small");
        eq(Json::number(NAN), "0", "NaN as 0");
        eq(Json::parse("[1.25]")->dump(), "[1.25]", "round trip");
        eq(Json::array().push(NAN).push(INFINITY).dump(), "[null,null]", "non-finite numbers dump as null");
        check(Json::parseDouble("12.5") == 12.5 && Json::parseDouble("7px") == 7 && Json::parseDouble("x") == 0,
              "parseDouble: the leading number, 0 when none");
        Json big = Json::parse("9007199254740993").value_or(Json());
        check(big.asInt64() == 9007199254740992LL || big.asInt64() == 9007199254740993LL, "big integer");
        check(big.asInt() == 2147483647 && Json(-1e12).asInt() == -2147483647 - 1, "asInt clamps to int");
        eq(Json(int64_t(-42)).dump(), "-42", "int64");
        std::setlocale(LC_NUMERIC, "C");
    }
    // quote()
    {
        eq(Json::quote("a\"b\\c\n\x01"), R"("a\"b\\c\n\u0001")", "quote escapes");
        eq(Json::quote("\xe2\x80\xa8"), "\"\xe2\x80\xa8\"", "quote keeps UTF-8");
    }
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
