// Profile 1 as data. Every table quotes the guide's item ("What widgets support", https://core.telegram.org/bots/widgets);
// "decision:" marks a detail the guide leaves open, decided by what TwidgetCore draws (SPEC S9.1).
#include "profile.h"

#include <algorithm>
#include <iterator>
#include <string>

namespace twcard::profile {

namespace {

template <size_t N> bool in(const std::string_view (&list)[N], std::string_view s) {
    return std::find(std::begin(list), std::end(list), s) != std::end(list);
}

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

// ── HTML ──

// "Text and structure: div, span, p, br, hr, headings h1–h6, header, footer, main, nav, section, article, aside, pre,
// blockquote, figure, figcaption."
// "Inline text elements: b, i, u, s, em, strong, small, mark, code, kbd, sub, sup, abbr, cite, q, del, ins, time, …."
//   decision (the "…"): also var, samp, dfn, bdi, bdo, data, wbr, address, hgroup, search — HTML's phrasing and
//   sectioning elements litehtml lays out like the listed ones.
// "Lists (ul, ol, li, dl, dt, dd, menu), tables with colspan and rowspan, details and summary."
//   decision: tables = table, caption, colgroup, col, thead, tbody, tfoot, tr, td, th.
// "<img> with a data: PNG, JPEG, GIF or WebP, <canvas>, <button>." "Links: <a href=…>".
// The document's own elements: html, head, body, style (CSS is in), plus svg (Inline SVG is in).
// SPEC S9.1 also keeps ruby and its base, annotation and fallback elements.
constexpr std::string_view kHtmlIn[] = {
    "html", "head", "body", "style",
    "div", "span", "p", "br", "hr", "h1", "h2", "h3", "h4", "h5", "h6", "header", "footer", "main", "nav", "section",
    "article", "aside", "pre", "blockquote", "figure", "figcaption",
    "b", "i", "u", "s", "em", "strong", "small", "mark", "code", "kbd", "sub", "sup", "abbr", "cite", "q", "del", "ins",
    "time", "var", "samp", "dfn", "bdi", "bdo", "data", "wbr", "address", "hgroup", "search",
    "ul", "ol", "li", "dl", "dt", "dd", "menu",
    "table", "caption", "colgroup", "col", "thead", "tbody", "tfoot", "tr", "td", "th",
    "details", "summary", "img", "canvas", "button", "a", "svg",
    "ruby", "rt", "rp", "rb",
};

// Out, "removed with their content": "Form controls: input, select, textarea, option, fieldset, …" (decision: the rest
// of HTML's form-associated elements: optgroup, legend, datalist, output, keygen); "progress, meter, dialog and
// popovers, audio, video, iframe, object, embed, external <script src>, link, meta, template, math".
//   decision: their companions go with them (source, track, picture, param, map, area, frame, frameset, noscript,
//   applet, portal), and `base` (a URL).
constexpr std::string_view kHtmlDrop[] = {
    "input", "select", "textarea", "option", "optgroup", "fieldset", "legend", "datalist", "output", "keygen",
    "progress", "meter", "dialog", "audio", "video", "iframe", "object", "embed", "link", "meta", "template", "math",
    "source", "track", "picture", "param", "map", "area", "base", "noscript",
};
constexpr std::string_view kHtmlDropMore[] = {"frame", "frameset", "applet", "portal"};

// Kept by the card world, never drawn: scripts run there (S1.5), `title` is the document's.
constexpr std::string_view kHtmlNotRendered[] = {"script", "title"};

// "<form> becomes a block and <label> an inline box." "Legacy presentational elements and attributes (center, font,
// align, bgcolor, …)": decision: their content stays, in a box without the presentation (center → div, the inline
// ones → span).
struct Rename {
    std::string_view from, to;
};
constexpr Rename kHtmlRename[] = {
    {"form", "div"}, {"label", "span"}, {"center", "div"}, {"font", "span"}, {"big", "span"}, {"tt", "span"},
    {"strike", "s"}, {"acronym", "abbr"}, {"marquee", "span"}, {"blink", "span"}, {"nobr", "span"}, {"listing", "pre"},
    {"xmp", "pre"},
};

// ── HTML attributes ──
// "Attributes: class, id, style, title, hidden, lang, dir, data-*, aria-*, and inline on* handlers (they run in the
// sandbox)": on* never reach the renderer (S9.1). Out: "contenteditable, draggable, tabindex, autofocus, accesskey",
// legacy presentational attributes.
constexpr std::string_view kGlobalAttributes[] = {"class", "id", "style", "title", "hidden", "lang", "dir"};
// decision: what the in elements need to be drawn as HTML draws them.
struct ElementAttributes {
    std::string_view tag;
    std::string_view names;  // space-separated
};
constexpr ElementAttributes kElementAttributes[] = {
    {"a", "href"},
    {"img", "src alt width height"},
    {"canvas", "width height"},
    {"button", "disabled type"},
    {"details", "open"},
    {"td", "colspan rowspan"},
    {"th", "colspan rowspan scope"},
    {"col", "span"},
    {"colgroup", "span"},
    {"ol", "start reversed type"},
    {"li", "value"},
    {"time", "datetime"},
    {"data", "value"},
    {"bdo", "dir"},
    {"abbr", "title"},
};

// ── SVG (guide: "Inline SVG") ──
// "In: shapes and paths, g, defs, symbol, use (fragment links), transform, viewBox, text and tspan, linear and radial
// gradients, CSS on SVG". decision: gradients include their `stop`s; `title`/`desc` are SVG's own text, not drawn.
constexpr std::string_view kSvgIn[] = {
    "svg", "g", "defs", "symbol", "use", "path", "rect", "circle", "ellipse", "line", "polyline", "polygon",
    "text", "tspan", "linearGradient", "radialGradient", "stop", "style",
};
constexpr std::string_view kSvgNotRendered[] = {"title", "desc"};
// Out: "clipPath, mask, pattern, marker, filter primitives, SMIL animation (animate, animateTransform, animateMotion),
// foreignObject, <image>". Everything else in SVG (textPath, switch, a, script, …) goes too: SVG has no inline box.

// SVG attributes. decision: geometry and the presentation attributes TwidgetCore's SVG draws.
constexpr std::string_view kSvgAttributes[] = {
    "id", "class", "style", "transform", "viewBox", "preserveAspectRatio", "x", "y", "width", "height", "cx", "cy", "r",
    "rx", "ry", "d", "points", "x1", "y1", "x2", "y2", "fx", "fy", "fr", "dx", "dy", "rotate", "offset",
    "gradientUnits", "gradientTransform", "spreadMethod", "pathLength", "textLength", "lengthAdjust", "href",
    "xlink:href", "xmlns", "xmlns:xlink", "lang", "hidden", "dir", "transform-origin",
};
// Presentation attributes: CSS values (S9.5 "SVG presentation attributes").
constexpr std::string_view kSvgPresentation[] = {
    "fill", "fill-opacity", "fill-rule", "stroke", "stroke-width", "stroke-opacity", "stroke-linecap", "stroke-linejoin",
    "stroke-miterlimit", "stroke-dasharray", "stroke-dashoffset", "opacity", "stop-color", "stop-opacity", "color",
    "display", "visibility", "text-anchor", "dominant-baseline", "font-size", "font-family", "font-weight",
    "font-style", "letter-spacing", "vector-effect", "paint-order", "transform-box",
};

constexpr std::string_view kUrlAttributes[] = {
    "href", "xlink:href", "src", "srcset", "ping", "action", "formaction", "background", "poster", "srcdoc", "data",
    "codebase", "manifest", "cite", "longdesc", "usemap",
};

// ── CSS ──

// Function names allowed in every value (lowercase). From the guide's CSS "In": colours "rgb(), hsl()" (and their
// -a forms), "light-dark()"; gradients "linear, radial and conic" (+ repeating-); values "calc(), min(), max(), clamp(),
// custom properties and var(), env(safe-area-inset-*)" (rewritten by the sanitizer), "attr(data-*) and counters" (in
// `content`); motion "cubic-bezier(), steps(), linear()"; layout grid: decision: repeat(), minmax(), fit-content();
// url() is checked by the sanitizer (data: images and #fragments only).
constexpr std::string_view kFunctions[] = {
    "rgb", "rgba", "hsl", "hsla", "light-dark", "linear-gradient", "radial-gradient", "conic-gradient",
    "repeating-linear-gradient", "repeating-radial-gradient", "repeating-conic-gradient", "calc", "min", "max", "clamp",
    "var", "env", "url", "cubic-bezier", "steps", "linear", "repeat", "minmax", "fit-content", "counter",
};
constexpr std::string_view kContentFunctions[] = {"counters", "attr"};
// "2D transforms (and the translate, rotate, scale properties)"; out: "3D transforms and perspective".
constexpr std::string_view kTransformFunctions[] = {
    "matrix", "translate", "translatex", "translatey", "scale", "scalex", "scaley", "rotate", "skew", "skewx", "skewy",
};
// "filter with blur(), brightness(), drop-shadow(), and backdrop-filter with blur(), brightness()".
constexpr std::string_view kFilterFunctions[] = {"blur", "brightness", "drop-shadow"};
constexpr std::string_view kBackdropFunctions[] = {"blur", "brightness"};

// "Values: px, em, rem, %, ch, ex, vw / vmin / vmax (the bubble's width)"; out: "vh and other height-relative units".
// decision: the absolute lengths (cm mm q in pt pc), which TwidgetCore draws; angles and times for transforms and
// motion; fr for grid tracks.
constexpr std::string_view kUnits[] = {
    "px", "em", "rem", "ch", "ex", "vw", "vmin", "vmax", "cm", "mm", "q", "in", "pt", "pc",
    "deg", "grad", "rad", "turn", "s", "ms", "fr",
};

// Properties: the guide's CSS "In" list, grouped as it groups them. decision: the longhands of each listed shorthand,
// and `-webkit-` spellings of listed ones that cards write (backdrop-filter, user-select).
constexpr PropertyRule kProperties[] = {
    // Layout: "block, inline, inline-block, flexbox (with gap), grid, tables, position: relative / absolute / fixed,
    // z-index, float, aspect-ratio, box-sizing, object-fit, overflow: hidden / clip".
    {"display", Value::Display},
    {"position", Value::Keywords, "static relative absolute fixed sticky=relative"},
    {"top"}, {"right"}, {"bottom"}, {"left"}, {"inset"},
    {"z-index"}, {"float", Value::Keywords, "none left right"}, {"clear", Value::Keywords, "none left right both"},
    {"aspect-ratio"}, {"box-sizing", Value::Keywords, "content-box border-box"},
    {"object-fit", Value::Keywords, "fill contain cover none scale-down"}, {"object-position"},
    {"overflow", Value::Keywords, "visible hidden clip auto=hidden scroll=hidden overlay=hidden"},
    {"overflow-x", Value::Keywords, "visible hidden clip auto=hidden scroll=hidden overlay=hidden"},
    {"overflow-y", Value::Keywords, "visible hidden clip auto=hidden scroll=hidden overlay=hidden"},
    {"width"}, {"height"}, {"min-width"}, {"min-height"}, {"max-width"}, {"max-height"},
    {"flex"}, {"flex-flow"}, {"flex-direction"}, {"flex-wrap"}, {"flex-grow"}, {"flex-shrink"}, {"flex-basis"},
    {"order"}, {"justify-content"}, {"justify-items"}, {"justify-self"}, {"align-content"}, {"align-items"},
    {"align-self"}, {"place-content"}, {"place-items"}, {"place-self"},
    {"gap"}, {"row-gap"}, {"column-gap"}, {"grid-gap"}, {"grid-row-gap"}, {"grid-column-gap"},
    {"grid"}, {"grid-template"}, {"grid-template-columns"}, {"grid-template-rows"}, {"grid-template-areas"},
    {"grid-auto-rows"}, {"grid-auto-columns"}, {"grid-auto-flow"}, {"grid-area"}, {"grid-row"}, {"grid-column"},
    {"grid-row-start"}, {"grid-row-end"}, {"grid-column-start"}, {"grid-column-end"},
    {"table-layout"}, {"border-collapse"}, {"border-spacing"}, {"caption-side"}, {"empty-cells"},
    // Box and paint: "margins, padding, borders (solid, dashed, dotted, double), border-radius, outline, box-shadow,
    // opacity, visibility, backgrounds with data: images, linear, radial and conic gradients, 2D transforms (and the
    // translate, rotate, scale properties)".
    {"margin"}, {"margin-top"}, {"margin-right"}, {"margin-bottom"}, {"margin-left"},
    {"padding"}, {"padding-top"}, {"padding-right"}, {"padding-bottom"}, {"padding-left"},
    {"border"}, {"border-top"}, {"border-right"}, {"border-bottom"}, {"border-left"},
    {"border-width"}, {"border-top-width"}, {"border-right-width"}, {"border-bottom-width"}, {"border-left-width"},
    {"border-style"}, {"border-top-style"}, {"border-right-style"}, {"border-bottom-style"}, {"border-left-style"},
    {"border-color"}, {"border-top-color"}, {"border-right-color"}, {"border-bottom-color"}, {"border-left-color"},
    {"border-radius"}, {"border-top-left-radius"}, {"border-top-right-radius"}, {"border-bottom-right-radius"},
    {"border-bottom-left-radius"},
    {"outline"}, {"outline-width"}, {"outline-style"}, {"outline-color"}, {"outline-offset"},
    {"box-shadow"}, {"opacity"}, {"visibility", Value::Keywords, "visible hidden collapse"},
    {"background"}, {"background-color"}, {"background-image"}, {"background-repeat"}, {"background-position"},
    {"background-position-x"}, {"background-position-y"}, {"background-size"}, {"background-origin"},
    {"background-clip", Value::Keywords, "border-box padding-box content-box"}, {"background-attachment"},
    {"transform", Value::Transform}, {"transform-origin"}, {"transform-box"}, {"translate"}, {"rotate"}, {"scale"},
    // Filters: "filter with blur(), brightness(), drop-shadow(), and backdrop-filter with blur(), brightness()".
    {"filter", Value::Filter}, {"backdrop-filter", Value::BackdropFilter}, {"-webkit-backdrop-filter", Value::BackdropFilter},
    // Text: "font-size, font-weight, font-style, line-height, the font shorthand, generic families only …,
    // font-variant-numeric: tabular-nums, text-align, text-decoration, text-transform, letter-spacing, white-space,
    // overflow-wrap, word-break, text-overflow: ellipsis, vertical-align, list styles, right-to-left text, color emoji,
    // user-select: none / text / auto".
    {"font", Value::Font}, {"font-family", Value::FontFamily}, {"font-size"}, {"font-weight"}, {"font-style"},
    {"line-height"}, {"font-variant-numeric", Value::Keywords, "normal tabular-nums"},
    {"text-align"}, {"text-decoration"}, {"text-decoration-line"}, {"text-decoration-style"},
    {"text-decoration-color"}, {"text-decoration-thickness"}, {"text-underline-offset"}, {"text-transform"},
    {"letter-spacing"}, {"word-spacing"}, {"white-space"}, {"overflow-wrap"}, {"word-wrap"}, {"word-break"},
    {"text-overflow", Value::Keywords, "clip ellipsis"}, {"vertical-align"}, {"text-indent"},
    {"list-style"}, {"list-style-type"}, {"list-style-position"}, {"list-style-image"},
    {"direction", Value::Keywords, "ltr rtl"}, {"unicode-bidi"},
    {"user-select", Value::Keywords, "none text auto"}, {"-webkit-user-select", Value::Keywords, "none text auto"},
    // Colors: "named, hex, rgb(), hsl(), transparent, currentColor, light-dark() and color-scheme".
    {"color"}, {"color-scheme", Value::Keywords, "normal light dark only"},
    // Motion: "transitions, @keyframes animations, timing functions, animation-play-state".
    {"transition"}, {"transition-property"}, {"transition-duration"}, {"transition-timing-function"},
    {"transition-delay"}, {"transition-behavior"},
    {"animation"}, {"animation-name"}, {"animation-duration"}, {"animation-timing-function"}, {"animation-delay"},
    {"animation-iteration-count"}, {"animation-direction"}, {"animation-fill-mode"}, {"animation-play-state"},
    {"animation-composition"},
    // Values: "content with strings, attr(data-*) and counters".
    {"content", Value::Content}, {"counter-reset"}, {"counter-increment"}, {"counter-set"}, {"quotes"},
    // "touch-action: auto / pan-y / none, pointer-events, cursor".
    {"touch-action", Value::TouchAction}, {"pointer-events"}, {"cursor"},
    // CSS on SVG: "fills and strokes from stylesheets, CSS animation of SVG properties".
    {"fill"}, {"fill-opacity"}, {"fill-rule"}, {"stroke"}, {"stroke-width"}, {"stroke-opacity"}, {"stroke-linecap"},
    {"stroke-linejoin"}, {"stroke-miterlimit"}, {"stroke-dasharray"}, {"stroke-dashoffset"}, {"stop-color"},
    {"stop-opacity"}, {"text-anchor"}, {"dominant-baseline"}, {"vector-effect"}, {"paint-order"},
    {"cx"}, {"cy"}, {"r"}, {"rx"}, {"ry"}, {"x"}, {"y"}, {"d"},
    // decision: rendering hints with no visual risk that TwidgetCore reads.
    {"-webkit-font-smoothing"}, {"will-change"}, {"isolation", Value::Keywords, "auto"},
};
// Out (guide): "Logical properties (margin-inline, …), multi-column layout", "3D transforms and perspective", "clip-path,
// masks, blend modes", "text-shadow, -webkit-line-clamp, background-clip: text, writing-mode": not in the table above.

// Selectors: "type, class, id, attribute, combinators, structural pseudo-classes (:first-child, :nth-child(), :not(),
// …), :is(), :where(), :root, ::before, ::after, :active, :hover (with a mouse only)"; ":disabled and :enabled work for
// <button>". Out: ":has(), :focus*, ::first-letter, ::first-line, ::marker, :target, and form-state selectors".
// decision ("…"): the structural pseudo-classes of Selectors 4 that litehtml matches.
constexpr std::string_view kPseudoClasses[] = {
    "first-child", "last-child", "only-child", "first-of-type", "last-of-type", "only-of-type", "empty", "root",
    "active", "hover", "disabled", "enabled", "before", "after",  // :before / :after: the CSS2 pseudo-element syntax
};
constexpr std::string_view kPseudoFunctions[] = {
    "nth-child", "nth-last-child", "nth-of-type", "nth-last-of-type", "not", "is", "where",
};
constexpr std::string_view kPseudoElements[] = {"before", "after"};

// "At-rules: @supports, @keyframes, and @media on the width, the orientation, prefers-color-scheme and
// prefers-reduced-motion." Out: "@font-face and named fonts, @import, @container, @layer, @property, CSS nesting",
// "@media on hover, pointer, resolution or print".
constexpr std::string_view kAtRules[] = {"media", "supports", "keyframes"};
constexpr std::string_view kMediaFeatures[] = {
    "width", "min-width", "max-width", "orientation", "prefers-color-scheme", "prefers-reduced-motion",
};
constexpr std::string_view kMediaTypes[] = {"all", "screen"};

// "generic families only (system-ui, sans-serif, serif, monospace, ui-sans-serif, ui-serif, ui-monospace, ui-rounded,
// with -apple-system turned into system-ui and other names removed)".
constexpr std::string_view kGenericFamilies[] = {
    "system-ui", "sans-serif", "serif", "monospace", "ui-sans-serif", "ui-serif", "ui-monospace", "ui-rounded",
};

} // namespace

ElementRule html_element(std::string_view tag) {
    if (in(kHtmlIn, tag)) return {Element::Allow};
    if (in(kHtmlDrop, tag) || in(kHtmlDropMore, tag)) return {Element::Drop};
    if (in(kHtmlNotRendered, tag)) return {Element::NotRendered};
    for (const Rename& r : kHtmlRename)
        if (r.from == tag) return {Element::Rename, r.to.data()};
    return {Element::Unknown, "span"};
}

ElementRule svg_element(std::string_view tag) {
    if (in(kSvgIn, tag)) return {Element::Allow};
    if (in(kSvgNotRendered, tag)) return {Element::NotRendered};
    return {Element::Drop};
}

bool html_attribute(std::string_view tag, std::string_view name) {
    if (in(kGlobalAttributes, name)) return true;
    if (starts_with(name, "data-") && name.size() > 5) return true;
    if (starts_with(name, "aria-") && name.size() > 5) return true;
    if (name == "role") return true;  // decision: kept with aria-* ("kept but do nothing", guide Design guidelines)
    for (const ElementAttributes& e : kElementAttributes) {
        if (e.tag != tag) continue;
        std::string_view names = e.names;
        size_t at = 0;
        while (at <= names.size()) {
            size_t sp = names.find(' ', at);
            std::string_view n = names.substr(at, sp == std::string_view::npos ? std::string_view::npos : sp - at);
            if (n == name) return true;
            if (sp == std::string_view::npos) break;
            at = sp + 1;
        }
    }
    return false;
}

bool svg_attribute(std::string_view tag, std::string_view name) {
    if (in(kSvgAttributes, name) || in(kSvgPresentation, name)) return true;
    if (starts_with(name, "data-") && name.size() > 5) return true;
    if (starts_with(name, "aria-") && name.size() > 5) return true;
    return false;
}

bool svg_presentation_attribute(std::string_view name) { return in(kSvgPresentation, name); }
bool url_attribute(std::string_view name) { return in(kUrlAttributes, name); }
bool visible_text_attribute(std::string_view name) {
    return name == "title" || name == "alt" || starts_with(name, "data-") || starts_with(name, "aria-");
}

const PropertyRule* property(std::string_view name) {
    for (const PropertyRule& p : kProperties)
        if (p.name == name) return &p;
    return nullptr;
}

bool function_allowed(std::string_view f, const PropertyRule& p) {
    if (in(kFunctions, f)) return true;
    switch (p.value) {
    case Value::Content: return in(kContentFunctions, f);
    case Value::Transform: return in(kTransformFunctions, f);
    case Value::Filter: return in(kFilterFunctions, f);
    case Value::BackdropFilter: return in(kBackdropFunctions, f);
    default: break;
    }
    // The individual transform properties take no functions of their own; drop-shadow() only in filter.
    return false;
}

bool unit_allowed(std::string_view unit) { return in(kUnits, unit); }

bool pseudo_class_allowed(std::string_view name, bool functional) {
    return functional ? in(kPseudoFunctions, name) : in(kPseudoClasses, name);
}
bool pseudo_element_allowed(std::string_view name) { return in(kPseudoElements, name); }

bool at_rule_allowed(std::string_view name) { return in(kAtRules, name); }
bool media_feature_allowed(std::string_view name) { return in(kMediaFeatures, name); }
bool media_type_allowed(std::string_view name) { return in(kMediaTypes, name); }

bool generic_family(std::string_view name) { return in(kGenericFamilies, name); }

bool image_mime_allowed(std::string_view mime) {
    return mime == "image/png" || mime == "image/jpeg" || mime == "image/gif" || mime == "image/webp";
}

} // namespace twcard::profile
