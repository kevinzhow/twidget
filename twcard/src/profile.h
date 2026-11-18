// Profile 1 (SPEC S9.1, decision D1): the public guide's "What widgets support" written down as data, item by item
// (https://core.telegram.org/bots/widgets, Bot API 10.4). Each table in profile.cpp quotes the guide's item it comes
// from; where the guide leaves a detail open the decision is ours, taken by what TwidgetCore draws, and is recorded
// next to the entry ("decision:").
//
// The validator (S9.3, S9.5), `CSS.supports` / `@supports` and TelegramWidget.profile all answer from this file.
#pragma once

#include <string_view>

namespace twcard::profile {

/// TelegramWidget.profile.
inline constexpr int kProfile = 1;

// ───────────────────────── HTML (guide: "What widgets support → HTML") ─────────────────────────

enum class Element {
    Allow,       // drawn as it is
    Rename,      // drawn as another element (renameTo)
    Unknown,     // "Unknown elements render as inline boxes": a span with data-tg-tag
    Drop,        // removed with its content
    NotRendered  // kept by the card world, never sent to the renderer (scripts, title)
};
struct ElementRule {
    Element rule;
    const char* renameTo = nullptr;
};
/// An HTML element by its lowercase local name.
ElementRule html_element(std::string_view tag);
/// An SVG element by its qualified-case local name (`linearGradient`). SVG has no inline box: unknown ones drop.
ElementRule svg_element(std::string_view tag);

/// Whether attribute `name` (lowercase) of HTML element `tag` passes (values are checked separately).
bool html_attribute(std::string_view tag, std::string_view name);
/// Whether attribute `name` (as written, e.g. `viewBox`, `xlink:href`) of SVG element `tag` passes.
bool svg_attribute(std::string_view tag, std::string_view name);
/// SVG presentation attributes: their values are CSS and are sanitised as a declaration of the same name (S9.5).
bool svg_presentation_attribute(std::string_view name);
/// Attributes whose value is a URL in HTML or SVG (never passed except where the validator rewrites them, S9.3).
bool url_attribute(std::string_view name);
/// Attributes counted as visible text (S9.6 "document text": "text, visible, data-* and aria-* attributes").
bool visible_text_attribute(std::string_view name);

// ───────────────────────── CSS (guide: "What widgets support → CSS") ─────────────────────────

/// How the sanitizer treats a property's value.
enum class Value {
    Any,           // generic checks only (functions, units, URLs)
    Keywords,      // one of `keywords` (or a CSS-wide keyword), after `rewrite`
    FontFamily,    // generic families only, -apple-system → system-ui
    Font,          // the `font` shorthand: the family list as FontFamily
    Transform,     // 2D transform functions only
    Filter,        // blur() brightness() drop-shadow()
    BackdropFilter,// blur() brightness()
    Content,       // strings, attr(data-*), counters, quotes
    Display,       // the guide's display values
    TouchAction,   // auto | pan-y | none
};
struct PropertyRule {
    std::string_view name;
    Value value = Value::Any;
    /// For Keywords: space-separated allowed keywords; `from=to` rewrites one (overflow:auto → hidden).
    std::string_view keywords = {};
};
/// The rule of a property (lowercase; vendor prefixes as written), or nullptr when it is out of the profile.
/// Custom properties (`--x`) are handled by the sanitizer (generic checks only).
const PropertyRule* property(std::string_view name);

/// Whether a function may appear in a value of `property` (lowercase function name, without the parenthesis).
bool function_allowed(std::string_view function, const PropertyRule& property);
/// Whether a dimension unit (lowercase) is in.
bool unit_allowed(std::string_view unit);

/// Selectors: pseudo-classes (`:hover`, functional `:nth-child(`) and pseudo-elements (`::before`), lowercase.
bool pseudo_class_allowed(std::string_view name, bool functional);
bool pseudo_element_allowed(std::string_view name);

/// At-rules kept (lowercase, without `@`): media, supports, keyframes.
bool at_rule_allowed(std::string_view name);
/// Media features (lowercase, with min-/max- prefixes) and media types.
bool media_feature_allowed(std::string_view name);
bool media_type_allowed(std::string_view name);

/// The generic font families (guide, Text), and whether `name` is one; `-apple-system` maps to `system-ui`.
bool generic_family(std::string_view name);

/// HTML profile: the image types of `data:` URLs that pass (guide: "a data: PNG, JPEG, GIF or WebP").
bool image_mime_allowed(std::string_view mime);

} // namespace twcard::profile
