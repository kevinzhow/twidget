// The CSS sanitizer (twcard/src/css_sanitizer.h) against SPEC S9.4/S9.5 and profile 1 (the guide's CSS lists), and the
// URL checks (urls.h). All inputs are ours; the hostile ones are the S9 security vectors that are CSS.
#include "css_sanitizer.h"
#include "urls.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using twcard::CssSanitizer;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) failures++;
}

std::map<std::string, int> g_diags;
CssSanitizer S([](std::string_view k, std::string_view) { g_diags[std::string(k)]++; });

std::string sheet(const std::string& css) {
    CssSanitizer::Counts c;
    return S.stylesheet(css, c);
}
std::string val(const std::string& prop, const std::string& v) { return S.value(prop, v).value_or("<dropped>"); }

void eq(const std::string& got, const std::string& want, const std::string& what) {
    check(got == want, what + (got == want ? "" : "\n    got:  " + got + "\n    want: " + want));
}

bool no_remote(const std::string& s) {
    // No URL other than data: and #fragment survives: no scheme-looking text inside url().
    size_t at = 0;
    while ((at = s.find("url(", at)) != std::string::npos) {
        std::string rest = s.substr(at + 4, 12);
        if (rest.rfind("\"data:", 0) != 0 && rest.rfind("\"#", 0) != 0) return false;
        at += 4;
    }
    return s.find("http") == std::string::npos;
}

// A 2×3 PNG header (only the header is read).
const std::string kPng2x3 = "iVBORw0KGgoAAAANSUhEUgAAAAIAAAADCAYAAAC56t6BAAAAC0lEQVR42mNgwAkAABsAAVmYPeoAAAAASUVORK5CYII=";

} // namespace

int main() {
    // ── URLs (S9.4) ──
    {
        auto v = twcard::urls::check("data:image/png;base64," + kPng2x3);
        check(v.kind == twcard::urls::Kind::Image && v.url.rfind("data:image/png;base64,", 0) == 0, "a small data: PNG passes");
        check(twcard::urls::check("  DATA:IMAGE/PNG;BASE64," + kPng2x3 + " ").kind == twcard::urls::Kind::Image, "case and whitespace as WHATWG URL");
        check(twcard::urls::check("#grad").kind == twcard::urls::Kind::Fragment, "#fragment");
        check(twcard::urls::check("http://x/a.png").kind == twcard::urls::Kind::Blocked, "http blocked");
        check(twcard::urls::check("https://x/a.png").kind == twcard::urls::Kind::Blocked, "https blocked");
        check(twcard::urls::check("//x/a.png").kind == twcard::urls::Kind::Blocked, "scheme-relative blocked");
        check(twcard::urls::check("javascript:alert(1)").kind == twcard::urls::Kind::Blocked, "javascript: blocked");
        check(twcard::urls::check("data:text/html,<b>x</b>").kind == twcard::urls::Kind::Blocked, "data:text/html blocked");
        check(twcard::urls::check("data:image/svg+xml,<svg/>").kind == twcard::urls::Kind::Blocked, "data: SVG out (guide)");
        check(twcard::urls::check("data:image/png;base64,AAAA").kind == twcard::urls::Kind::Placeholder, "unreadable PNG: placeholder");
        check(twcard::urls::check("da\tta:image/png;base64," + kPng2x3).kind == twcard::urls::Kind::Image, "tab removed as WHATWG URL does");
        // 1025×1 PNG header
        std::vector<uint8_t> big = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 4, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0, 0, 0, 0, 0};
        auto b = twcard::urls::check("data:image/png;base64," + twcard::urls::base64_encode(big.data(), big.size()));
        check(b.kind == twcard::urls::Kind::Placeholder && b.url == twcard::urls::kPlaceholderImage, "1025-px image: placeholder");
        auto dec = twcard::urls::forgiving_base64_decode(" aGk= ");
        check(dec && std::string(dec->begin(), dec->end()) == "hi" && !twcard::urls::forgiving_base64_decode("a") &&
                  !twcard::urls::forgiving_base64_decode("a$bc"),
              "forgiving base64");
    }

    // ── values ──
    eq(val("color", "red"), "red", "a plain value");
    eq(val("COLOR", " #FFF "), "#FFF", "property case, whitespace");
    eq(val("background", "url(http://evil/x.png) red"), "<dropped>", "url(http…) drops the declaration");
    eq(val("background", "url( \"ht\\9 tp://evil\" )"), "<dropped>", "escaped scheme in a string");
    eq(val("background", "\\75 rl(http://evil)"), "<dropped>", "escaped function name \\75 rl(");
    eq(val("background", "image-set(\"http://evil\" 1x)"), "<dropped>", "image-set()");
    eq(val("background", "-webkit-image-set(url(http://e) 1x)"), "<dropped>", "-webkit-image-set()");
    eq(val("background-image", "url(var(--u))"), "<dropped>", "var() inside url()");
    eq(val("background-image", "url(attr(data-u))"), "<dropped>", "attr() inside url()");
    eq(val("background-image", "url(\"#frag\")"), "url(\"#frag\")", "url(#fragment) kept");
    {
        std::string v = val("background-image", "url(data:image/png;base64," + kPng2x3 + ")");
        check(v.rfind("url(\"data:image/png;base64,", 0) == 0, "a data image in CSS is kept, re-encoded (" + v.substr(0, 40) + ")");
    }
    eq(val("fill", "url(#g)"), "url(\"#g\")", "SVG paint server by fragment");
    eq(val("filter", "url(#f)"), "<dropped>", "filter: url() (filter primitives are out)");
    eq(val("filter", "blur(2px) brightness(1.2)"), "blur(2px) brightness(1.2)", "filter functions in");
    eq(val("filter", "grayscale(1)"), "<dropped>", "other filter functions out");
    eq(val("backdrop-filter", "drop-shadow(0 0 2px red)"), "<dropped>", "drop-shadow only in filter");
    eq(val("transform", "translate(10px, 5px) rotate(45deg)"), "translate(10px, 5px) rotate(45deg)", "2D transforms");
    eq(val("transform", "rotateX(45deg)"), "<dropped>", "3D transforms out");
    eq(val("transform", "perspective(100px)"), "<dropped>", "perspective out");
    eq(val("rotate", "x 45deg"), "<dropped>", "rotate with an axis out");
    eq(val("height", "50vh"), "<dropped>", "vh out");
    eq(val("width", "50vw"), "50vw", "vw in");
    eq(val("width", "calc(100% - 2em)"), "calc(100% - 2em)", "calc");
    eq(val("color", "oklch(70% 0.1 200)"), "<dropped>", "modern colour functions out");
    eq(val("color", "color-mix(in srgb, red, blue)"), "<dropped>", "color-mix out");
    eq(val("color", "light-dark(#000, #fff)"), "light-dark(#000, #fff)", "light-dark in");
    eq(val("overflow", "auto"), "hidden", "overflow: auto → hidden");
    eq(val("overflow", "hidden scroll"), "hidden hidden", "two values");
    eq(val("position", "sticky"), "relative", "sticky → relative");
    eq(val("display", "contents"), "<dropped>", "display: contents out");
    eq(val("display", "flex"), "flex", "flex in");
    eq(val("touch-action", "pan-x"), "<dropped>", "touch-action: pan-x out");
    eq(val("touch-action", "none"), "none", "touch-action: none");
    eq(val("font-family", "-apple-system, Helvetica, \"Segoe UI\", sans-serif"), "system-ui,sans-serif", "families: generic only");
    eq(val("font-family", "Arial"), "<dropped>", "no generic family left");
    eq(val("font", "bold 14px/1.2 Arial, monospace"), "bold 14px/1.2 monospace", "font shorthand families");
    eq(val("font", "14px Arial"), "14px system-ui", "font shorthand with no generic family");
    eq(val("padding-bottom", "max(8px, env(safe-area-inset-bottom))"), "max(8px, var(--tg-content-safe-area-inset-bottom))", "env() safe area");
    eq(val("padding", "env(safe-area-inset-left, 4px)"), "var(--tg-content-safe-area-inset-left, 4px)", "env() with a fallback");
    eq(val("padding", "env(keyboard-inset-height, 3px)"), "3px", "other env(): its fallback");
    eq(val("padding", "env(titlebar-area-x)"), "<dropped>", "other env() without a fallback");
    eq(val("content", "attr(data-x) \"!\" counter(c)"), "attr(data-x) \"!\" counter(c)", "content: attr(data-*), counters");
    eq(val("content", "attr(href)"), "<dropped>", "attr() of another attribute");
    eq(val("content", "url(data:image/png;base64," + kPng2x3 + ")"), "<dropped>", "content images out");
    eq(val("width", "attr(data-w px)"), "<dropped>", "attr() outside content");
    eq(val("text-shadow", "1px 1px red"), "<dropped>", "text-shadow out");
    eq(val("clip-path", "circle(50%)"), "<dropped>", "clip-path out");
    eq(val("margin-inline", "4px"), "<dropped>", "logical properties out");
    eq(val("border", "2px groove red"), "<dropped>", "groove borders out");
    eq(val("border", "2px dashed red"), "2px dashed red", "dashed borders in");
    eq(val("box-shadow", "inset 0 0 2px red"), "inset 0 0 2px red", "inset box-shadow is not a border style");
    eq(val("--x", "url(http://evil)"), "<dropped>", "custom property with a remote url");
    eq(val("--x", "{ a b }"), "<dropped>", "custom property with a block");
    eq(val("--accent", "#3390ec"), "#3390ec", "custom property");
    eq(val("--blur", "blur(3px)"), "blur(3px)", "custom property holding a filter");
    eq(val("display", "var(--d)"), "var(--d)", "var() kept (checked where substituted)");
    eq(val("color", "inherit"), "inherit", "CSS-wide keywords");
    eq(val("font-variant-numeric", "tabular-nums"), "tabular-nums", "tabular-nums");
    eq(val("font-variant-numeric", "oldstyle-nums"), "<dropped>", "other numeric variants out");
    eq(val("color", "re\\64"), "red", "escaped ident written back unescaped");
    eq(val("grid-template-areas", "\"a b\" \"c d\""), "\"a b\" \"c d\"", "strings");
    eq(val("width", "1e3px"), "1e3px", "exponent number");
    {
        std::string deep;
        for (int i = 0; i < 5000; i++) deep += "calc(";
        deep += "1px";
        for (int i = 0; i < 5000; i++) deep += ")";
        check(val("width", deep) == "<dropped>", "deeply nested (((…))) CSS: dropped, no crash");
    }

    // ── declaration lists (style attributes) ──
    eq(S.declarationList("color: red; background: url(http://x); width: 10px !important; behavior: url(x.htc)"),
       "color: red; width: 10px !important", "style attribute: bad declarations dropped");
    eq(S.declarationList("background-image: url(data:image/png;base64,AAA;B); color: red").find("color: red") != std::string::npos ? "ok" : "no",
       "ok", "a ; inside url() does not split declarations (tokenizer-based)");

    // ── stylesheets ──
    eq(sheet("@import url(http://evil/x.css); a{color:red}"), "a{color:red;}", "@import dropped");
    eq(sheet("@font-face{font-family:x;src:url(http://evil/f.woff)} b{color:red}"), "b{color:red;}", "@font-face dropped");
    eq(sheet("@namespace svg url(http://www.w3.org/2000/svg); i{color:red}"), "i{color:red;}", "@namespace dropped");
    eq(sheet("a:has(b){color:red} a:hover,a:focus{color:blue} p::first-line{x:y} q::before{content:\"x\"}"),
       "a:hover{color:blue;}q::before{content:\"x\";}", "selectors out of profile dropped one by one");
    eq(sheet(".a{color:red; .b{color:blue}}"), ".a{color:red;}", "CSS nesting dropped");
    eq(sheet("div > p + ul ~ li [data-x=\"1\"] #i.c *{color:red}"), "div > p + ul ~ li [data-x=\"1\"] #i.c *{color:red;}", "combinators, attributes");
    eq(sheet("li:nth-child(2n+1):not(.x){color:red}"), "li:nth-child(2n+1):not(.x){color:red;}", "structural pseudo-classes");
    eq(sheet("@media print{a{color:red}} @media (hover: hover){b{color:red}}"), "", "@media print / hover dropped");
    eq(sheet("@media (min-width: 300px) and (prefers-color-scheme: dark){a{color:red}}"),
       "@media (min-width: 300px) and (prefers-color-scheme: dark){a{color:red;}}", "@media on width and the scheme");
    eq(sheet("@media screen and (orientation: portrait), print{a{color:red}}"), "@media screen and (orientation: portrait){a{color:red;}}",
       "a list keeps the queries that can match");
    eq(sheet("@media (width >= 300px){a{color:red}}"), "@media (width >= 300px){a{color:red;}}", "range syntax");
    eq(sheet("@supports (display: grid){a{display:grid}} @supports (display: contents){b{display:contents}}"), "a{display:grid;}",
       "@supports decided from the profile");
    eq(sheet("@supports not (text-shadow: 1px 1px red){a{color:red}}"), "a{color:red;}", "@supports not");
    eq(sheet("@keyframes spin{from{transform:rotate(0)}to{transform:rotate(360deg);animation-name:x;opacity:1 !important}}"),
       "@keyframes spin{from{transform:rotate(0);}to{transform:rotate(360deg);}}", "@keyframes");
    eq(sheet("@keyframes k{50%{background:url(http://evil)}}"), "@keyframes k{50%{}}", "keyframe value with url() dropped");
    eq(sheet("@layer x{a{color:red}} @container (width>1px){a{color:red}} @property --x{syntax:'*'}"), "", "@layer, @container, @property dropped");
    {
        CssSanitizer::Counts c;
        S.stylesheet("a{content:\"héllo\"} @media (min-width:1px){b{color:red} c{color:red}} @keyframes x{} @keyframes y{}", c);
        check(c.rules == 3 && c.keyframes == 2 && c.stringUnits == 5, "counts: rules (inside @media too), @keyframes, string units");
    }
    {
        std::string css;
        for (int i = 0; i < 3000; i++) css += "{";
        CssSanitizer::Counts c;
        S.stylesheet(css, c);
        check(true, "3000 open braces: no crash");
    }
    check(no_remote(sheet("a{background:url('http://x')} b{background:url(\"data:image/png;base64," + kPng2x3 + "\")} c{list-style-image:url(https://x)}")),
          "no non-data URL survives a stylesheet");
    eq(sheet("a{background:URL(  http://x  )}"), "", "URL() unquoted, any case");
    eq(sheet("a{color:red}</style><img src=http://x>"), "a{color:red;}", "markup after a rule is no CSS");

    // ── supports() and media queries, the API forms ──
    check(S.supportsDeclaration("display", "grid") && !S.supportsDeclaration("display", "contents"), "CSS.supports(prop, value)");
    check(!S.supportsDeclaration("overflow", "auto") && S.supportsDeclaration("overflow", "hidden"), "a rewritten value is not kept");
    check(S.supportsCondition("(display: flex) and (gap: 4px)") && !S.supportsCondition("(display: flex) and (text-shadow: none)"),
          "CSS.supports(condition)");
    check(S.supportsCondition("display: flex"), "CSS.supports with a bare declaration");
    check(S.supportsCondition("selector(:is(a, b))") && !S.supportsCondition("selector(:has(a))"), "selector()");
    check(S.mediaQueryList("(prefers-color-scheme: dark)") == std::optional<std::string>("(prefers-color-scheme: dark)") &&
              !S.mediaQueryList("(pointer: coarse)"),
          "matchMedia queries");

    // ── easing (anim records) ──
    check(S.easing("ease-in") == std::optional<std::string>("ease-in") && S.easing("cubic-bezier(0.1, 2, 0.3, -1)") &&
              !S.easing("cubic-bezier(2, 0, 0, 0)") && S.easing("steps(4, jump-end)") && !S.easing("steps(0)") &&
              S.easing("linear(0, 0.5 50%, 1)") && !S.easing("url(x)"),
          "easing functions");

    // ── serializer ──
    eq(twcard::css_serialize_identifier("1a"), "\\31 a", "identifier starting with a digit");
    eq(twcard::css_serialize_identifier("a b"), "a\\ b", "space escaped");
    eq(twcard::css_serialize_identifier("-"), "\\-", "lone hyphen");
    eq(twcard::css_serialize_string("a\"b\\c\n"), "\"a\\\"b\\\\c\\a \"", "string escapes");
    eq(val("width", "calc(1px + -2px)"), "calc(1px + -2px)", "numbers keep their sign");

    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
