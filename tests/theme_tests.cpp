// The root variables TwidgetCore sets from the host's `theme` and `safeArea` messages (core/src/css.h), against
// SPEC S7.1 and S10.2: the 15 Mini Apps theme keys as `--tg-theme-*`, `#rrggbb` only (lowercased), missing or bad
// values from our default palette per scheme; the content safe area as six `--tg-content-safe-area-*` px values.
#include "css.h"

#include <cstdio>
#include <map>
#include <set>
#include <string>

using twidget::Json;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) failures++;
}

std::map<std::string, std::string> vars_of(const Json& theme) {
    std::map<std::string, std::string> m;
    for (auto& [k, v] : twlite::css::theme_variables(theme)) m[k] = v;
    return m;
}

bool hex6_lower(const std::string& v) {
    if (v.size() != 7 || v[0] != '#') return false;
    for (char c : v.substr(1))
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

const char* kKeys[] = {"bg_color", "text_color", "hint_color", "link_color", "button_color", "button_text_color", "secondary_bg_color",
                       "header_bg_color", "bottom_bar_bg_color", "accent_text_color", "section_bg_color", "section_header_text_color",
                       "section_separator_color", "subtitle_text_color", "destructive_text_color"};

std::string var(const char* key) {
    std::string n = std::string("--tg-theme-") + key;
    for (char& c : n)
        if (c == '_') c = '-';
    return n;
}

} // namespace

int main() {
    auto light = vars_of(Json::object().set("colorScheme", "light"));
    auto dark = vars_of(Json::object().set("colorScheme", "dark"));
    {
        bool all = true, differ = 0;
        int different = 0;
        for (const char* k : kKeys) {
            all = all && hex6_lower(light[var(k)]) && hex6_lower(dark[var(k)]);
            different += light[var(k)] != dark[var(k)];
        }
        (void)differ;
        check(all, "every one of the 15 keys has a default in both schemes, #rrggbb lowercase");
        check(different >= 10, "the dark palette is another palette");
        check(light["--tg-color-scheme"] == "light" && dark["--tg-color-scheme"] == "dark", "--tg-color-scheme");
        check(vars_of(Json::object())["--tg-color-scheme"] == "light" && vars_of(Json::object().set("colorScheme", "DARK"))["--tg-color-scheme"] == "light",
              "anything but exactly \"dark\" is light");
        check(light["--tg-theme-bg-color"] != light["--tg-theme-text-color"], "text stands out from the background");
    }
    {
        Json params = Json::object()
                          .set("bg_color", "#ABCDEF")
                          .set("text_color", "#123")
                          .set("hint_color", "red")
                          .set("link_color", 5)
                          .set("button_color", "#00ff00 ")
                          .set("unknown_key", "#010203");
        auto v = vars_of(Json::object().set("colorScheme", "dark").set("params", params));
        check(v["--tg-theme-bg-color"] == "#abcdef", "a valid value is used, lowercased");
        check(v["--tg-theme-text-color"] == dark["--tg-theme-text-color"], "#rgb is not accepted");
        check(v["--tg-theme-hint-color"] == dark["--tg-theme-hint-color"], "a colour name is not accepted");
        check(v["--tg-theme-link-color"] == dark["--tg-theme-link-color"], "a number is not accepted");
        check(v["--tg-theme-button-color"] == dark["--tg-theme-button-color"], "trailing space is not accepted");
        check(!v.count("--tg-theme-unknown-key"), "other keys are dropped");
    }
    {
        std::set<std::string> names;
        for (auto& [k, v] : twlite::css::theme_variables(Json::object())) names.insert(k);
        bool all = true;
        for (const char* k : kKeys) all = all && names.count(var(k)) && twlite::css::is_theme_variable(var(k));
        check(all && twlite::css::is_theme_variable("--tg-color-scheme") && !twlite::css::is_theme_variable("--tg-theme-x"),
              "is_theme_variable knows every variable the theme sets");
    }
    // Content safe area (SPEC S7.1).
    {
        Json inset = Json::object().set("top", 12.345).set("right", 65).set("bottom", -1).set("left", "8").set("cornerLeft", 200).set("cornerRight", 200.5);
        std::map<std::string, std::string> v;
        for (auto& [k, x] : twlite::css::content_safe_area_variables(inset)) v[k] = x;
        check(v.size() == 6, "six safe-area variables");
        check(v["--tg-content-safe-area-inset-top"] == "12.35px" || v["--tg-content-safe-area-inset-top"] == "12.34px", "two decimals");
        check(v["--tg-content-safe-area-inset-right"] == "0px", "an inset over 64 is 0");
        check(v["--tg-content-safe-area-inset-bottom"] == "0px", "a negative inset is 0");
        check(v["--tg-content-safe-area-inset-left"] == "0px", "a string is 0");
        check(v["--tg-content-safe-area-corner-left"] == "200px", "a corner up to 200");
        check(v["--tg-content-safe-area-corner-right"] == "0px", "a corner over 200 is 0");
        bool known = true;
        for (auto& [k, x] : v) known = known && twlite::css::is_theme_variable(k);
        check(known, "the safe-area variables are reserved too");
    }
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
