// The CSS sanitizer (SPEC S9.5): every piece of CSS from a card (each <style> text, style attributes, `style` ops, SVG
// presentation attributes, keyframe values of `anim` records, canvas font/colour/filter strings) is parsed with our
// litehtml fork's CSS Syntax 3 tokenizer and parser — the renderer's own, so there is no parser differential — checked
// against profile 1 (profile.h) and written out again from the parsed tokens. Nothing passes verbatim, and what the
// parser cannot make sense of is dropped.
//
// Diagnostics: `profile` (out of profile), `css-dropped` (could not be parsed), `url-blocked` (a URL other than a data:
// image or a #fragment).
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace twcard {

class CssSanitizer {
public:
    using Diag = std::function<void(std::string_view kind, std::string_view detail)>;

    /// What a stylesheet or declaration list contributes to the counted caps (S9.6).
    struct Counts {
        int rules = 0;            // style rules (inside @media / @supports too)
        int keyframes = 0;        // @keyframes rules
        size_t stringUnits = 0;   // UTF-16 units of CSS strings ("document text")
    };

    explicit CssSanitizer(Diag diag = nullptr) : diag_(std::move(diag)) {}

    /// A <style> element's text.
    std::string stylesheet(std::string_view css, Counts& counts) const;
    /// A `style` attribute: a declaration list, written back as `name: value; …`.
    std::string declarationList(std::string_view css, Counts* counts = nullptr) const;
    /// One declaration's value (a `style` op, an SVG presentation attribute, a keyframe value): the value to send,
    /// nullopt when the declaration is dropped. `property` is matched lowercase (custom properties as they are).
    std::optional<std::string> value(std::string_view property, std::string_view value, Counts* counts = nullptr) const;

    /// @supports and CSS.supports (guide, "Detecting features": "true only when the app keeps that declaration").
    bool supportsDeclaration(std::string_view property, std::string_view value) const;
    bool supportsCondition(std::string_view condition) const;

    /// A media query list from matchMedia: the re-serialised list, nullopt when no query in it can match in profile 1.
    std::optional<std::string> mediaQueryList(std::string_view text) const;

    /// A CSS <easing-function> (S9.7 `anim` easing): re-serialised, or nullopt.
    std::optional<std::string> easing(std::string_view text) const;

    void diag(std::string_view kind, std::string_view detail) const {
        if (diag_) diag_(kind, detail);
    }

private:
    Diag diag_;
};

/// CSSOM "serialize an identifier" / "serialize a string" (CSSOM §2.1, common serializing idioms).
std::string css_serialize_identifier(std::string_view ident);
std::string css_serialize_string(std::string_view s);

} // namespace twcard
