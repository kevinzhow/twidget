#include "twidget/theme.h"

#include <algorithm>
#include <string>

namespace twidget {
namespace {

struct ThemeColor {
    const char* key;
    const char* light;
    const char* dark;
};

// SPEC S7.1: every key has a default for both chat color schemes.
constexpr ThemeColor colors[] = {
    {"bg_color", "#ffffff", "#1e2228"},
    {"text_color", "#1b1d21", "#f3f4f6"},
    {"hint_color", "#868c94", "#9097a0"},
    {"link_color", "#2481cc", "#6ab3f3"},
    {"button_color", "#2a8bd8", "#5aa5e6"},
    {"button_text_color", "#ffffff", "#ffffff"},
    {"secondary_bg_color", "#f0f2f5", "#15181c"},
    {"header_bg_color", "#f7f8fa", "#22272e"},
    {"bottom_bar_bg_color", "#f7f8fa", "#22272e"},
    {"accent_text_color", "#2481cc", "#6ab3f3"},
    {"section_bg_color", "#ffffff", "#1e2228"},
    {"section_header_text_color", "#4a8fcf", "#79b6ee"},
    {"section_separator_color", "#e2e5e9", "#30353d"},
    {"subtitle_text_color", "#80868e", "#8a9099"},
    {"destructive_text_color", "#d9443c", "#ef6259"},
};

bool hexColor(const std::string& value) {
    return value.size() == 7 && value.front() == '#' &&
           std::all_of(value.begin() + 1, value.end(), [](char digit) {
               return (digit >= '0' && digit <= '9') || (digit >= 'A' && digit <= 'F') ||
                      (digit >= 'a' && digit <= 'f');
           });
}

} // namespace

Json theme_params(const Json& params, bool dark) {
    Json result = Json::object();
    for (const auto& color : colors) {
        std::string value = params[color.key].asString();
        if (hexColor(value)) {
            for (char& c : value)
                if (c >= 'A' && c <= 'F') c += 'a' - 'A';
        } else {
            value = dark ? color.dark : color.light;
        }
        result.set(color.key, std::move(value));
    }
    return result;
}

Json content_safe_area(const Json& inset) {
    Json result = Json::object();
    for (const std::string_view key : {"top", "right", "bottom", "left", "cornerLeft", "cornerRight"}) {
        const double maximum = key.starts_with("corner") ? 200 : 64;
        const double value = inset[key].asNumber();
        result.set(key, value >= 0 && value <= maximum ? value : 0);
    }
    return result;
}

} // namespace twidget
