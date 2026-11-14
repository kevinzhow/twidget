#include "renderer.h"

#include "computed_style.h"
#include "css.h"
#include "tw_ua_stylesheet.h"

#include <litehtml.h>
#include <litehtml/html_tag.h>
#include <litehtml/render_inline_context.h>
#include <litehtml/render_item.h>
#ifdef TW_LITEHTML_TWIDGET
#include <litehtml/css_effects.h>
#include <litehtml/el_break.h>
#include <litehtml/el_comment.h>
#include <litehtml/el_svg.h>
#include <litehtml/el_text.h>
#include <litehtml/render_inline.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>

using twidget::Json;

namespace twlite {

namespace {

/// widget-sandbox's shared bubble bounds are 50 to 600 CSS px; the host may raise the top (set_max_height).
constexpr double MIN_HEIGHT = 50.0;
/// The tallest a host may let a widget grow: past this a document is a runaway, not content.
constexpr double HEIGHT_CEILING = 16384.0;
constexpr int MAX_PASSES = 4;
/// Sent whether or not the widget listens. The one-off animation events are among them because a page's
/// first build starts its animations before its script says what it listens for; animationiteration
/// repeats, and stays opt-in.
const char* const ALWAYS_FORWARD[] = {"click", "load", "error", "toggle", "animationstart", "animationend", "transitionend"};
/// Every event the widget may get (profile 1, `tables.events` in widget-sandbox's profile.json): no keys,
/// focus, input, touch, wheel or scroll; dblclick and contextmenu only as the gesture policy decides.
const char* const PROFILE_EVENTS[] = {
    "click", "pointerdown", "pointerup", "pointermove", "pointerenter", "pointerleave", "pointerover", "pointerout", "pointercancel",
    "dblclick", "contextmenu",
    "mousedown", "mouseup", "mousemove", "mouseenter", "mouseleave", "mouseover", "mouseout",
    "load", "error", "toggle", "animationstart", "animationend", "animationiteration", "transitionend",
};
/// ::before and ::after have no node of their own: the animator knows them by these ids, negative so they
/// never meet a widget's. Owner 12's ::before is -24, its ::after -25.
constexpr int64_t pseudo_id(int64_t owner, bool after) { return -(owner * 2 + (after ? 1 : 0)); }
constexpr int64_t pseudo_owner(int64_t id) { return id < 0 ? -id / 2 : id; }
constexpr bool pseudo_after(int64_t id) { return id < 0 && (-id) % 2 == 1; }

/// The layer of an element's box-shadow pulse (update_composition) has an id of its own: the element's
/// with this bit, under the element's own layer.
constexpr int64_t RING_BIT = int64_t(1) << 50;

/// One outer box-shadow as `x y blur spread colour` in px (what the animator samples), or nothing.
struct Shadow1 {
    float x = 0, y = 0, blur = 0, spread = 0;
    Color color;
};
std::optional<Shadow1> parse_shadow(std::string_view text) {
    std::vector<std::string> parts;
    int depth = 0;
    std::string cur;
    for (char c : text) {
        if (c == '(') depth++;
        if (c == ')') depth--;
        if (c == ',' && depth == 0) return std::nullopt;  // more than one shadow
        if (std::isspace((unsigned char)c) && depth == 0) {
            if (!cur.empty()) parts.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(std::move(cur));
    Shadow1 out;
    std::vector<float> lengths;
    bool colour = false;
    for (const std::string& part : parts) {
        std::string_view v = part;
        if (css::ends_with(v, "px")) v.remove_suffix(2);
        if (auto n = css::number(v)) {
            lengths.push_back(float(*n));
        } else if (!colour && css::parse_color(part, out.color)) {
            colour = true;
        } else {
            return std::nullopt;  // inset, other units
        }
    }
    if (lengths.size() < 2 || lengths.size() > 4 || !colour) return std::nullopt;
    out.x = lengths[0];
    out.y = lengths[1];
    out.blur = lengths.size() > 2 ? lengths[2] : 0;
    out.spread = lengths.size() > 3 ? lengths[3] : 0;
    return out;
}

/// A font's metrics as Chromium lays lines out with them (SimpleFontData::PlatformInit and
/// FontMetrics::AscentDescentWithHacks): the ascent and the descent rounded to whole device pixels; with subpixel
/// text positioning, which Chromium uses on Linux and Android above a device scale of 1, a descent rounded down takes
/// a pixel from the ascent (so descenders are not cut); the line spacing (line-height: normal) is the rounded ascent
/// and descent plus the rounded line gap. With the half-leadings floored to a device pixel (litehtml's line_box) and
/// each text line's top on a device pixel (el_text) this puts text where Chromium paints it. Before, text with a
/// line-height under ascent + descent sat up to 1 px lower than in Chromium (the recipe card's − and + in 30 px
/// circles: 18 px Roboto at line-height 1, 1.03 px at a DPR of 3).
FontMetrics line_metrics(FontMetrics m, float dpr) {
    const float k = dpr > 0 ? dpr : 1;
    const float gap = std::max(0.0f, m.height - m.ascent - m.descent);
    float ascent = std::round(m.ascent * k), descent = std::round(m.descent * k);
    if (k > 1 && descent < m.descent * k && ascent >= 1) {
        ascent -= 1;
        descent += 1;
    }
    m.ascent = ascent / k;
    m.descent = descent / k;
    m.height = (ascent + descent + std::round(gap * k)) / k;
    return m;
}

/// A script's focus() takes the keyboard only this soon after the user's input (as Swift's scriptFocusWindow).
constexpr double SCRIPT_FOCUS_S = 1;

/// Chromium/WebKit UA rules litehtml's master.css lacks and widgets rely on. Form controls get a
/// visible box: litehtml has no controls of its own (the container would own them), so they are
/// bordered inline blocks here, with the value as text where the shadow tree knows it.
///
/// The profile's UA layer (TW_UA_STYLESHEET, `tables.uaStylesheet` in widget-sandbox's profile.json) comes
/// after these: the root, the text elements and `<button>` on the theme's variables, as every renderer draws them.
const char* const UA_EXTRA = R"css(
button,input,select,textarea,meter,progress{box-sizing:border-box}
input,select,textarea{display:inline-block;border:1px solid #8f8f8f;border-radius:3px;background:#fff;color:#000;padding:2px 6px;font:13.333px -apple-system,system-ui,sans-serif;min-height:1.8em;min-width:2em;vertical-align:middle}
:where(button){display:inline-block;text-align:center}
input[type=checkbox],input[type=radio]{min-width:13px;width:13px;height:13px;min-height:13px;padding:0;margin:3px 3px 3px 4px;vertical-align:middle}
input[type=radio]{border-radius:50%}
input[type=range]{border:none;background:transparent;padding:0}
input[type=color]{width:50px;height:27px;padding:2px}
input[type=hidden]{display:none}
textarea{display:block;min-height:3em;white-space:pre-wrap;overflow-wrap:break-word}
progress,meter{display:inline-block;width:10em;height:1em;background:#e6e6e6;border-radius:2px}
canvas{display:inline-block;background:transparent}
svg{display:inline-block}
details{display:block}
details>summary:first-of-type{display:list-item;list-style:disclosure-closed inside;cursor:pointer}
details[open]>summary:first-of-type{list-style-type:disclosure-open}
details:not([open])>*:not(summary){display:none}
dialog:not([open]){display:none}
[dir=rtl]{direction:rtl}
[dir=ltr]{direction:ltr}
q::before{content:open-quote}
q::after{content:close-quote}
[hidden]{display:none}
)css";

const char* const VOID_TAGS[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"};

bool is_void(const std::string& tag) {
    for (const char* v : VOID_TAGS)
        if (tag == v) return true;
    return false;
}

bool starts_with_on(const std::string& name) {
    return name.size() >= 2 && (name[0] == 'o' || name[0] == 'O') && (name[1] == 'n' || name[1] == 'N');
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

void escape_text(const std::string& s, std::string& out) {
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out.push_back(c);
        }
    }
}

void escape_attr(const std::string& s, std::string& out) {
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '"': out += "&quot;"; break;
        default: out.push_back(c);
        }
    }
}

Color color_of(const litehtml::web_color& c) {
    return Color{c.red, c.green, c.blue, c.alpha};
}

/// Where letter-spacing goes: after each character, with combining marks, variation selectors, emoji
/// modifiers and ZWJ sequences kept on the character before them.
std::vector<std::string_view> spacing_units(std::string_view text) {
    std::vector<std::string_view> units;
    size_t start = 0;
    bool joined = false;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = text[i];
        size_t n = std::min<size_t>(c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4, text.size() - i);
        uint32_t cp = c < 0x80 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
        for (size_t k = 1; k < n; k++) cp = (cp << 6) | (uint8_t(text[i + k]) & 0x3F);
        bool extends = (cp >= 0x300 && cp <= 0x36F) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
                       (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F) ||
                       (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0020 && cp <= 0xE007F) || cp == 0x200D;
        if (i > 0 && !extends && !joined) {
            units.push_back(text.substr(start, i - start));
            start = i;
        }
        joined = cp == 0x200D;
        i += n;
    }
    if (start < text.size()) units.push_back(text.substr(start));
    return units;
}

/// The spaces word-spacing widens: U+0020 and U+00A0.
size_t spacing_spaces(std::string_view text) {
    size_t n = 0;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == ' ') n++;
        else if (uint8_t(text[i]) == 0xC2 && i + 1 < text.size() && uint8_t(text[i + 1]) == 0xA0) n++;
    }
    return n;
}

/// `text` cut around the spaces word-spacing widens, each space on its own.
std::vector<std::string_view> spacing_words(std::string_view text) {
    std::vector<std::string_view> words;
    size_t start = 0;
    for (size_t i = 0; i < text.size();) {
        size_t n = text[i] == ' ' ? 1 : uint8_t(text[i]) == 0xC2 && i + 1 < text.size() && uint8_t(text[i + 1]) == 0xA0 ? 2 : 0;
        if (!n) {
            i++;
            continue;
        }
        if (i > start) words.push_back(text.substr(start, i - start));
        words.push_back(text.substr(i, n));
        i += n;
        start = i;
    }
    if (start < text.size()) words.push_back(text.substr(start));
    return words;
}

Rect rect_of(const litehtml::position& p) {
    return Rect{float(p.x), float(p.y), float(p.width), float(p.height)};
}

/// The line boxes of an inline element, in document coordinates; none for any other element.
std::vector<Rect> inline_boxes_of(const litehtml::element::ptr& el) {
    std::vector<Rect> boxes;
    auto ri = el->get_render_item();
    if (!ri) return boxes;
    // They are kept in the frame the item's own position is in: its placement, less that position.
    litehtml::position placed = ri->get_placement();
    float ox = float(placed.x - ri->pos().x), oy = float(placed.y - ri->pos().y);
    ri->for_inline_boxes([&](const litehtml::position& b, bool, bool) {
        if (float(b.width) > 0 || float(b.height) > 0) boxes.push_back(Rect{ox + float(b.x), oy + float(b.y), float(b.width), float(b.height)});
        return true;
    });
    return boxes;
}

#ifdef TW_LITEHTML_TWIDGET
/// A path's bounds, sampled along its outline so that curves bound by their ink, not their control points.
bool path_bounds(const litehtml::vector_path& path, float& x0, float& y0, float& x1, float& y1) {
    const int steps = 512;
    float total = litehtml::svg_path_length(path, -1, nullptr, nullptr);
    x0 = y0 = INFINITY;
    x1 = y1 = -INFINITY;
    for (int i = 0; i <= steps; i++) {
        float x = 0, y = 0;
        litehtml::svg_path_length(path, total * float(i) / steps, &x, &y);
        x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
    }
    return std::isfinite(x0);
}

/// An SVG shape or text has no box of its own: its geometry, or its text, through its transforms and the view box.
std::optional<Rect> svg_box_of(const litehtml::element::ptr& el) {
    litehtml::transform_matrix ctm;
    if (el->get_render_item() || !litehtml::svg_ctm(el, ctm)) return std::nullopt;
    float x0, y0, x1, y1;
    litehtml::vector_path path;
    litehtml::position text;
    if (litehtml::svg_geometry(el, path)) {
        if (!path_bounds(path, x0, y0, x1, y1)) return std::nullopt;
    } else if (litehtml::svg_text_box(el, el->get_document()->container(), text)) {
        x0 = float(text.x); y0 = float(text.y); x1 = x0 + float(text.width); y1 = y0 + float(text.height);
    } else {
        return std::nullopt;
    }
    float bx0 = INFINITY, by0 = INFINITY, bx1 = -INFINITY, by1 = -INFINITY;
    for (auto [x, y] : {std::pair{x0, y0}, std::pair{x1, y0}, std::pair{x0, y1}, std::pair{x1, y1}}) {
        float tx = ctm.a * x + ctm.c * y + ctm.tx, ty = ctm.b * x + ctm.d * y + ctm.ty;
        bx0 = std::min(bx0, tx); by0 = std::min(by0, ty); bx1 = std::max(bx1, tx); by1 = std::max(by1, ty);
    }
    return Rect{bx0, by0, bx1 - bx0, by1 - by0};
}
#endif

/// The border box of an element in document coordinates (`get_placement()` is the content box;
/// getBoundingClientRect / offsetWidth are border boxes).
Rect border_box_of(const litehtml::element::ptr& el) {
#ifdef TW_LITEHTML_TWIDGET
    if (auto svg = svg_box_of(el)) return *svg;
#endif
    // An inline element shows as its line boxes; its own placement is its containing block's origin.
    std::vector<Rect> lines = inline_boxes_of(el);
    if (!lines.empty()) {
        float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
        for (const Rect& l : lines) {
            x0 = std::min(x0, l.x); y0 = std::min(y0, l.y); x1 = std::max(x1, l.x + l.w); y1 = std::max(y1, l.y + l.h);
        }
        return Rect{x0, y0, x1 - x0, y1 - y0};
    }
    litehtml::position pos = el->get_placement();
    Rect r = rect_of(pos);
    if (auto ri = el->get_render_item()) {
        const auto& pad = ri->get_paddings();
        const auto& bd = ri->get_borders();
        r.x -= float(pad.left) + float(bd.left);
        r.y -= float(pad.top) + float(bd.top);
        r.w += float(pad.left) + float(pad.right) + float(bd.left) + float(bd.right);
        r.h += float(pad.top) + float(pad.bottom) + float(bd.top) + float(bd.bottom);
    }
    return r;
}

/// How far the content under `ri` reaches, for scrollWidth and scrollHeight: in-flow children by their
/// margin boxes, everything else by its border box, and nothing past a descendant that clips.
void content_extent(const std::shared_ptr<litehtml::render_item>& ri, float& right, float& bottom, bool children) {
    for (const auto& c : ri->children()) {
        const auto& el = c->src_el();
        if (!el || el->css().get_display() == litehtml::display_none) continue;
        Rect b = border_box_of(el);
        float r = b.x + b.w, bt = b.y + b.h;
        auto position = el->css().get_position();
        if (children && (position == litehtml::element_position_static || position == litehtml::element_position_relative)) {
            r += float(c->get_margins().right);
            bt += float(c->get_margins().bottom);
        }
        right = std::max(right, r);
        bottom = std::max(bottom, bt);
        if (el->css().get_overflow() == litehtml::overflow_visible) content_extent(c, right, bottom, false);
    }
}

/// Where an element's border box shows: its transform and its ancestors', as a bounding box.
/// Where `r`, in `el`'s layout coordinates, shows: its transform and its ancestors', as a bounding box.
Rect transformed_box(const litehtml::element::ptr& el, Rect r) {
#ifdef TW_LITEHTML_TWIDGET
    litehtml::transform_matrix m;
    for (auto ri = el->get_render_item(); ri; ri = ri->parent()) m = m.then(ri->get_visual_transform());
    if (m.is_identity()) return r;
    float xs[4] = {r.x, r.x + r.w, r.x, r.x + r.w}, ys[4] = {r.y, r.y, r.y + r.h, r.y + r.h};
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (int i = 0; i < 4; i++) {
        float x = m.a * xs[i] + m.c * ys[i] + m.tx, y = m.b * xs[i] + m.d * ys[i] + m.ty;
        x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
    }
    return Rect{x0, y0, x1 - x0, y1 - y0};
#else
    return r;
#endif
}

/// The border box a script sees (getBoundingClientRect, offsetWidth, ...): border_box_of, except that a cell of a
/// border-collapse table owns half of each collapsed border, as in CSS 2 §17.6.2 and Chromium. litehtml lays such
/// cells out with their whole borders overlapping their neighbours' (the same grid, each cell 1 border wider).
namespace {
/// The collapsed-border cells of the table `cell` is in, every row group's rows in order.
void table_cells(const litehtml::element::ptr& cell, std::vector<litehtml::element::ptr>& out) {
    auto row = cell->parent();
    auto group = row ? row->parent() : nullptr;
    auto table = group && group->css().get_display() != litehtml::display_table ? group->parent() : group;
    if (!table) return;
    std::function<void(const litehtml::element::ptr&)> walk = [&](const litehtml::element::ptr& e) {
        for (const auto& c : e->children()) {
            if (c->css().get_display() == litehtml::display_table_cell) out.push_back(c);
            else if (c->css().get_display() != litehtml::display_table) walk(c);
        }
    };
    walk(table);
}
} // namespace

Rect script_box_of(const litehtml::element::ptr& el) {
    Rect r = border_box_of(el);
    auto ri = el->get_render_item();
    if (ri && el->css().get_display() == litehtml::display_table_cell && el->css().get_border_collapse() == litehtml::border_collapse_collapse) {
        const auto& bd = ri->get_borders();
        float left = float(bd.left), top = float(bd.top), right = float(bd.right), bottom = float(bd.bottom);
        // CSS 2 §17.6.2: each cell's box takes half of each shared border. litehtml overlaps the cells by their own
        // borders; where a side has none of its own, the neighbour's (a row above with only a border-bottom) lies
        // outside the box, and half of it belongs to this cell.
        float out_l = 0, out_t = 0, out_r = 0, out_b = 0;
        if (left == 0 || top == 0 || right == 0 || bottom == 0) {
            std::vector<litehtml::element::ptr> cells;
            table_cells(el, cells);
            const float eps = 0.75f;
            for (const auto& c : cells) {
                if (c == el) continue;
                auto cri = c->get_render_item();
                if (!cri) continue;
                Rect o = border_box_of(c);
                const auto& ob = cri->get_borders();
                bool rows = o.x < r.x + r.w - eps && o.x + o.w > r.x + eps, cols = o.y < r.y + r.h - eps && o.y + o.h > r.y + eps;
                if (top == 0 && rows && std::fabs(o.y + o.h - r.y) < eps) out_t = std::max(out_t, float(ob.bottom));
                if (bottom == 0 && rows && std::fabs(r.y + r.h - o.y) < eps) out_b = std::max(out_b, float(ob.top));
                if (left == 0 && cols && std::fabs(o.x + o.w - r.x) < eps) out_l = std::max(out_l, float(ob.right));
                if (right == 0 && cols && std::fabs(r.x + r.w - o.x) < eps) out_r = std::max(out_r, float(ob.left));
            }
        }
        r.x += left / 2 - out_l / 2;
        r.y += top / 2 - out_t / 2;
        r.w -= (left + right) / 2 - (out_l + out_r) / 2;
        r.h -= (top + bottom) / 2 - (out_t + out_b) / 2;
    }
    return r;
}

Rect visual_box_of(const litehtml::element::ptr& el) {
    return transformed_box(el, script_box_of(el));
}

#ifdef TW_LITEHTML_TWIDGET
Rect padding_box_of(const litehtml::render_item* ri) {
    Rect border = border_box_of(ri->src_el());
    float l = float(ri->border_left()), t = float(ri->border_top()), r = float(ri->border_right()), b = float(ri->border_bottom());
    return Rect{border.x + l, border.y + t, border.w - l - r, border.h - t - b};
}

/// What can show of an item's border box: inside the padding box of each ancestor that clips it,
/// up to a fixed ancestor, which escapes them.
Rect clipped_border_box(const litehtml::render_item* ri) {
    Rect box = border_box_of(ri->src_el());
    bool escaped = ri->src_el()->css().get_position() == litehtml::element_position_fixed;
    for (auto p = ri->parent(); p && box.w > 0 && box.h > 0; p = p->parent()) {
        if (escaped || !p->parent()) break;
        escaped = p->src_el()->css().get_position() == litehtml::element_position_fixed;
        if (!p->src_el()->css().clips_contents()) continue;
        Rect clip = padding_box_of(p.get());
        float x0 = std::max(box.x, clip.x), y0 = std::max(box.y, clip.y);
        float x1 = std::min(box.x + box.w, clip.x + clip.w), y1 = std::min(box.y + box.h, clip.y + clip.h);
        box = Rect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }
    return box;
}

/// offsetLeft/offsetTop and offsetParent: from the border box to the padding edge of the nearest
/// positioned, transformed or contained ancestor (a table cell for a static element), else the body.
void offsets_of(const std::shared_ptr<litehtml::render_item>& ri, double& left, double& top, int64_t& parent) {
    auto origin = [](std::shared_ptr<litehtml::render_item> item, double& x, double& y) {
        x = y = 0;
        for (; item; item = item->parent()) {
            auto sticky = item->sticky_offset();
            x += float(item->pos().x) + float(sticky.x);
            y += float(item->pos().y) + float(sticky.y);
            if (item->css().get_position() == litehtml::element_position_fixed) break;
        }
    };
    origin(ri, left, top);
    left -= float(ri->padding_left()) + float(ri->border_left());
    top -= float(ri->padding_top()) + float(ri->border_top());
    parent = 0;
    auto tag = ri->src_el()->tag();
    if (ri->css().get_position() == litehtml::element_position_fixed || tag == litehtml::_body_ || tag == litehtml::_html_) return;
    for (auto p = ri->parent(); p; p = p->parent()) {
        const auto& css = p->css();
        auto ptag = p->src_el()->tag();
        bool positioned = css.get_position() != litehtml::element_position_static || !css.get_transform().empty() || css.contains(litehtml::contain_layout | litehtml::contain_paint);
        bool table = ri->css().get_position() == litehtml::element_position_static && (ptag == litehtml::_table_ || ptag == litehtml::_td_ || ptag == litehtml::_th_);
        if (!positioned && !table && ptag != litehtml::_body_) continue;
        if (const char* id = p->src_el()->get_attr("data-twid")) parent = std::strtoll(id, nullptr, 10);
        if (positioned || table) {
            double x, y;
            origin(p, x, y);
            left -= x - float(p->padding_left());
            top -= y - float(p->padding_top());
        }
        break;
    }
    left = std::round(left);
    top = std::round(top);
}
#endif

Radii radii_of(const litehtml::border_radiuses& r) {
    return Radii{float(r.top_left_x), float(r.top_left_y), float(r.top_right_x), float(r.top_right_y),
                 float(r.bottom_right_x), float(r.bottom_right_y), float(r.bottom_left_x), float(r.bottom_left_y)};
}

/// The radii of a background layer's clip (CSS Backgrounds 3 §5.3): a layer clipped to the padding or content box
/// follows that box's corner curves, the border radii less the widths between the border box and the clip box. Before,
/// such a layer took the outer radii, so a padding-box fill over a border-box gradient (a gradient border) covered the
/// gradient at the corners.
Radii layer_radii(const litehtml::background_layer& layer) {
    Radii r = radii_of(layer.border_radius);
    Rect b = rect_of(layer.border_box), c = rect_of(layer.clip_box);
    float left = c.x - b.x, top = c.y - b.y, right = (b.x + b.w) - (c.x + c.w), bottom = (b.y + b.h) - (c.y + c.h);
    if (left <= 0 && top <= 0 && right <= 0 && bottom <= 0) return r;
    auto less = [](float v, float by) { return std::max(0.0f, v - std::max(0.0f, by)); };
    return Radii{less(r.tlx, left), less(r.tly, top), less(r.trx, right), less(r.try_, top),
                 less(r.brx, right), less(r.bry, bottom), less(r.blx, left), less(r.bly, bottom)};
}

/// The shape a background layer fills: its border box, or its clip box when that is smaller (with layer_radii).
Rect layer_shape(const litehtml::background_layer& layer) {
    Rect b = rect_of(layer.border_box), c = rect_of(layer.clip_box);
    return c.x > b.x || c.y > b.y || c.x + c.w < b.x + b.w || c.y + c.h < b.y + b.h ? c : b;
}

Json rect_json(const Rect& r) {
    Json o = Json::object();
    o.set("x", r.x).set("y", r.y).set("width", r.w).set("height", r.h);
    o.set("top", r.y).set("left", r.x).set("right", r.x + r.w).set("bottom", r.y + r.h);
    return o;
}

std::atomic<double> g_test_clock{std::numeric_limits<double>::quiet_NaN()};  // seconds, NaN = steady clock

double now_ms() {
    using namespace std::chrono;
    double held = g_test_clock.load(std::memory_order_relaxed);
    if (!std::isnan(held)) return held * 1000;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

/// The bounding box of `r` mapped through `m`.
Rect bounds_under(const Rect& r, const Affine& m) {
    float xs[4] = {r.x, r.x + r.w, r.x, r.x + r.w}, ys[4] = {r.y, r.y, r.y + r.h, r.y + r.h};
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < 4; i++) {
        float x = m.apply_x(xs[i], ys[i]), y = m.apply_y(xs[i], ys[i]);
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

/// When a track's key at `t` (0..1) applies. A loop's end is the next iteration's start already, so
/// its last key reads a moment before.
double plan_time(const Animator::Plan& plan, double t) {
    if (plan.repeat && t >= 1) t = 1 - 1e-6;
    return plan.start + t * plan.duration;
}

Rect intersection(const Rect& a, const Rect& b) {
    float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    float x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
    return Rect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

bool contains(const Rect& outer, const Rect& inner) {
    constexpr float slack = 0.01f;
    return inner.x >= outer.x - slack && inner.y >= outer.y - slack && inner.x + inner.w <= outer.x + outer.w + slack && inner.y + inner.h <= outer.y + outer.h + slack;
}

/// What a shimmer layer animates: its background tile's position along x.
bool is_shimmer_property(const std::string& property) { return property == "background-position" || property == "background-position-x"; }

/// Where a background-position value puts a tile `tile` wide in `area`, along x; and along y when the
/// value has a second component, which must leave the tile where `tile` has it.
std::optional<float> shimmer_x(const std::string& value, const Rect& area, const Rect& tile) {
    auto length = [](const std::string& token, float room) -> std::optional<float> {
        if (token == "left" || token == "top") return 0.0f;
        if (token == "center") return room / 2;
        if (token == "right" || token == "bottom") return room;
        char* end = nullptr;
        float v = std::strtof(token.c_str(), &end);
        if (end == token.c_str()) return std::nullopt;
        std::string unit(end);
        if (unit == "%") return room * v / 100;
        if (unit == "px" || (unit.empty() && v == 0)) return v;
        return std::nullopt;
    };
    std::vector<std::string> tokens;
    for (size_t at = 0; at < value.size();) {
        size_t start = value.find_first_not_of(' ', at);
        if (start == std::string::npos) break;
        size_t stop = value.find(' ', start);
        tokens.push_back(value.substr(start, stop == std::string::npos ? std::string::npos : stop - start));
        at = stop == std::string::npos ? value.size() : stop;
    }
    if (tokens.empty() || tokens.size() > 2) return std::nullopt;
    auto x = length(tokens[0], area.w - tile.w);
    if (!x) return std::nullopt;
    if (tokens.size() == 2) {
        auto y = length(tokens[1], area.h - tile.h);
        if (!y || std::fabs(area.y + *y - tile.y) > 0.5f) return std::nullopt;
    }
    return area.x + *x;
}

/// An element of an SVG drawing below its outermost <svg> (which is a box of the HTML layout).
template <class N> bool svg_inside(const N* n) { return n->svg && n->parent && n->parent->svg; }

/// What a compositor layer animates; an element animating anything else is painted per frame.
bool is_layer_property(const std::string& property) {
    return property == "transform" || property == "opacity" || property == "translate" || property == "rotate" || property == "scale";
}

/// left and top move an absolutely positioned box without changing it or anything around it: a layer
/// plays them as a translation (Container::offset_frame says when).
bool is_offset_property(const std::string& property) { return property == "left" || property == "top"; }

/// The `style` attribute is the inline style itself, as in the DOM: setting it replaces what the
/// style ops set, and a style op that removes a property removes it from what the attribute set.
void set_inline_style(std::vector<std::pair<std::string, std::string>>& styles, const std::string& text) {
    styles.clear();
    for (auto& d : css::parse_declarations(text)) styles.emplace_back(std::move(d.name), d.important ? d.value + " !important" : std::move(d.value));
}

/// Move-type events: only the latest one per type and pointer goes out, once per frame.
bool coalesced(const std::string& type) {
    return type == "pointermove" || type == "mousemove" || type == "touchmove" || type == "wheel";
}

constexpr int MAX_CANVASES = 64;
/// Layers: at most this many per widget, this many device pixels in all, and no track shorter
/// than this (a frame or two is cheaper to paint than to hand over).
constexpr size_t MAX_LAYERS = 256;
/// Static elements painted over a wanted layer that may become layers of their own to let it be one.
constexpr size_t MAX_OBSCURERS = 8;
constexpr double MAX_LAYER_PIXELS = 4096.0 * 4096.0;
constexpr double MIN_LAYER_SPAN = 0.1;
/// An element moved by style ops in three batches running becomes a scripted layer: batches at most
/// this many apart, or this close in time, continue a streak.
constexpr uint64_t SCRIPTED_STREAK_BATCHES = 4;
constexpr double SCRIPTED_STREAK_GAP_S = 0.12;
constexpr double MAX_SCRIPTED_AREA = 2e6;
constexpr float LAYER_MARGIN = 48;
/// After user input, ops batches build at once: they likely answer it.
constexpr double INPUT_IMMEDIATE_S = 0.25;
/// The sandbox hands a long flush over in parts of this many ops (widget-sandbox v0.6.0, capture.ts PARTIAL_BATCH_OPS);
/// only the last part of a task is shorter.
constexpr size_t PARTIAL_BATCH_OPS = 1000;
/// The render throttle (the Swift renderer's): a pass costlier than a frame holds the next one back
/// for THROTTLE_FACTOR times its cost, 25 % of the thread at most; under sustained load, more than
/// SUSTAINED_LIMIT of SUSTAINED_WINDOW with two passes over HEAVY_PASS_S, 5 %. Nothing is charged in
/// the first LOAD_GRACE_S, while a widget sets itself up.
constexpr double THROTTLE_FREE_S = 0.016;
constexpr double THROTTLE_FACTOR = 3;
constexpr double SUSTAINED_THROTTLE_FACTOR = 19;
constexpr double SUSTAINED_WINDOW_S = 2;
constexpr double SUSTAINED_LIMIT = 0.1;
constexpr double HEAVY_PASS_S = 0.05;
constexpr double LOAD_GRACE_S = 1;
/// Backend fonts kept for documents to come, beyond those a document holds.
constexpr size_t MAX_FACES = 256;
/// The SVG paints the container keeps for draw_cached, and the commands they hold together.
constexpr size_t MAX_CACHED_PAINTS = 128;
constexpr size_t MAX_CACHED_COMMANDS = 50000;
/// Measured widths, keyed by their text: longer text is measured each time, and every face's keys
/// together stay within the byte bound.
constexpr size_t MAX_WIDTH_ENTRIES = 4096;
constexpr size_t MAX_WIDTH_KEY = 256;
constexpr size_t MAX_WIDTH_BYTES = 1 << 20;
/// Past this a selectable run's carets are spread evenly over its width rather than measured: a press
/// or a move must not shape a long unbroken run once per probe.
constexpr size_t MAX_MEASURED_RUN = 4096;
/// The profile's image types (widget-sandbox's PROFILE_IMAGES). The validator lets no other through, but
/// a path around it (generated content's url(), once) must not hand a backend anything else.
constexpr std::string_view IMAGE_TYPES[] = {"image/png", "image/jpeg", "image/jpg", "image/gif", "image/webp"};

/// A data: URL's media type as widget-sandbox's checkUrl reads it: the scheme in any case, around tabs,
/// newlines and the C0 controls and spaces at its ends; trimmed and lower-cased. Empty when not a data: URL.
std::string data_mime(std::string_view raw) {
    std::string url;
    for (char c : raw)
        if (c != '\t' && c != '\n' && c != '\r') url += c;
    size_t from = 0, to = url.size();
    while (from < to && uint8_t(url[from]) <= 0x20) from++;
    while (to > from && uint8_t(url[to - 1]) <= 0x20) to--;
    std::string_view u = std::string_view(url).substr(from, to - from);
    if (u.size() < 5 || lower(std::string(u.substr(0, 5))) != "data:") return {};
    size_t end = u.find_first_of(",;", 5);
    if (end == std::string_view::npos || u.find(',', 5) == std::string_view::npos) return {};
    return css::trim(lower(std::string(u.substr(5, end - 5))));
}

} // namespace

// ───────────────────────── color scheme rewrite ─────────────────────────

std::string rewrite_color_scheme(const std::string& css, bool dark) {
    static constexpr std::string_view KEY = "prefers-color-scheme";
    std::string out;
    out.reserve(css.size());
    size_t pos = 0;
    // The media feature's name as a whole identifier, outside strings, comments and URLs.
    for (size_t i = 0; i + KEY.size() <= css.size();) {
        if (size_t skip = css::skip_opaque(css, i); skip != i) {
            i = skip;
            continue;
        }
        if ((i > 0 && css::ident_char(css[i - 1])) || !css::iequals(std::string_view(css).substr(i, KEY.size()), KEY) ||
            (i + KEY.size() < css.size() && css::ident_char(css[i + KEY.size()]))) {
            i++;
            continue;
        }
        out.append(css, pos, i - pos);
        size_t j = i + KEY.size();
        while (j < css.size() && (css[j] == ' ' || css[j] == '\t' || css[j] == '\n' || css[j] == ':')) j++;
        size_t k = j;
        while (k < css.size() && std::isalpha((unsigned char)css[k])) k++;
        std::string value = lower(css.substr(j, k - j));
        bool matches = value.empty() ? true : (value == "dark") == dark;  // bare `(prefers-color-scheme)` is true
        out += matches ? "min-width:0px" : "min-width:999999px";
        pos = i = k;
    }
    out.append(css, pos, std::string::npos);
    return out;
}

// ───────────────────────── shadow tree ─────────────────────────

struct Renderer::Node {
    enum Kind { Element, Text, Comment } kind = Element;
    int64_t id = 0;  // widget node id, 0 until adopted/registered
    std::string tag;
    bool svg = false;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<std::pair<std::string, std::string>> styles;  // inline style properties set by `style` ops
    std::string data;                                         // text / comment data; stylesheet text of <style>
    Node* parent = nullptr;
    std::vector<Node*> children;
};

#ifdef TW_LITEHTML_TWIDGET
/// Where overflow-wrap may break a word that does not fit: between user-perceived characters, measured
/// through the container like any other text. Clusters are approximated from code points (combining
/// marks, variation selectors, skin tones, tags and ZWJ sequences stay together; regional indicators
/// pair up), which is all the fork asks of a platform without a grapheme API.
class TextBreaker final : public litehtml::text_breaker {
public:
    TextBreaker(litehtml::document_container& measure, const std::string& text, litehtml::uint_ptr font)
        : measure_(measure), text_(text), font_(font) {
        boundaries_.push_back(0);
        bool ascii = std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 0x20 && c < 0x7f; });
        if (ascii) {
            for (size_t i = 1; i <= text.size(); i++) boundaries_.push_back(i);
            return;
        }
        auto decode = [&](size_t at, uint32_t& cp) {
            unsigned char c = (unsigned char)text[at];
            size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
            if (at + n > text.size()) n = 1;
            cp = n == 1 ? c : n == 2 ? c & 0x1f : n == 3 ? c & 0x0f : c & 0x07;
            for (size_t k = 1; k < n; k++) cp = (cp << 6) | ((unsigned char)text[at + k] & 0x3f);
            return n;
        };
        auto extends = [](uint32_t cp) {
            return (cp >= 0x300 && cp <= 0x36f) || (cp >= 0x483 && cp <= 0x489) || (cp >= 0x591 && cp <= 0x5bd) || (cp >= 0x610 && cp <= 0x61a) ||
                   (cp >= 0x64b && cp <= 0x65f) || (cp >= 0x900 && cp <= 0x903) || (cp >= 0x93a && cp <= 0x94f) || (cp >= 0x1ab0 && cp <= 0x1aff) ||
                   (cp >= 0x1dc0 && cp <= 0x1dff) || (cp >= 0x200c && cp <= 0x200d) || (cp >= 0x20d0 && cp <= 0x20ff) || (cp >= 0xfe00 && cp <= 0xfe0f) ||
                   (cp >= 0xfe20 && cp <= 0xfe2f) || (cp >= 0x1f3fb && cp <= 0x1f3ff) || (cp >= 0xe0020 && cp <= 0xe007f) || (cp >= 0xe0100 && cp <= 0xe01ef);
        };
        auto regional = [](uint32_t cp) { return cp >= 0x1f1e6 && cp <= 0x1f1ff; };
        uint32_t previous = 0;
        int regionals = 0;
        for (size_t at = 0; at < text.size();) {
            uint32_t cp = 0;
            size_t n = decode(at, cp);
            bool joined = at > 0 && (extends(cp) || previous == 0x200d || (regional(cp) && regionals % 2 == 1));
            regionals = regional(cp) ? regionals + 1 : 0;
            if (at > 0 && !joined) boundaries_.push_back(at);
            previous = cp;
            at += n;
        }
        boundaries_.push_back(text.size());
    }

    size_t fit(size_t start, litehtml::pixel_t width) override {
        auto first = std::lower_bound(boundaries_.begin(), boundaries_.end(), start);
        if (first == boundaries_.end() || first + 1 == boundaries_.end()) return start;
        // At least one character goes on the line, however narrow it is.
        size_t lo = size_t(first - boundaries_.begin()) + 1, hi = boundaries_.size() - 1;
        if (float(width) <= 0 || measure(start, boundaries_[lo]) > float(width)) return boundaries_[lo];
        while (lo < hi) {
            size_t mid = (lo + hi + 1) / 2;
            if (measure(start, boundaries_[mid]) <= float(width)) lo = mid;
            else hi = mid - 1;
        }
        return boundaries_[lo];
    }

    litehtml::pixel_t minimum_width() override {
        if (minimum_ < 0) {
            minimum_ = 0;
            for (size_t i = 1; i < boundaries_.size(); i++) minimum_ = std::max(minimum_, measure(boundaries_[i - 1], boundaries_[i]));
        }
        return litehtml::pixel_t(minimum_);
    }

private:
    float measure(size_t from, size_t to) {
        return float(measure_.text_width(text_.substr(from, to - from).c_str(), font_));
    }

    litehtml::document_container& measure_;
    std::string text_;
    litehtml::uint_ptr font_;
    std::vector<size_t> boundaries_;
    float minimum_ = -1;
};
#endif

// ───────────────────────── litehtml container ─────────────────────────

class Container : public litehtml::document_container {
public:
    Container(Renderer& r, Backend& backend) : r_(r), backend_(backend) {}
    ~Container() override {
        // The document releases its fonts through this container on destruction: drop it
        // (and the element handles that keep its nodes alive) while the font table still exists.
        elements_.clear();
        doc_ = nullptr;
        for (auto& [key, face] : faces_) backend_.delete_font(face->handle);
    }

    // document lifecycle
    /// True when the last build patched the previous document instead of parsing a new one.
    bool patched() const { return patched_; }

    /// Boxes one layout may lay out afresh: nested flex, grid and table boxes measure their children several times
    /// each, so a few hundred bytes of markup ask for 2^depth layouts. The heaviest widget of widget-sandbox's corpus
    /// lays out 2,600 boxes in a pass; the base is 40 times that, and a document of many elements gets more.
    static constexpr uint64_t kLayoutBudgetBase = 100000;
    static constexpr uint64_t kLayoutBudgetPerElement = 50;

    /// Lays the document out within the layout budget. Past it the document stays as far as it got and lays out
    /// nothing again, and the renderer stops (Renderer::stop_layout).
    void layout(float width) {
        uint64_t elements = 0;
        std::vector<const litehtml::element*> pending;
        if (auto root = doc_->root()) pending.push_back(root.get());
        while (!pending.empty()) {
            const litehtml::element* el = pending.back();
            pending.pop_back();
            elements++;
            for (const auto& child : el->children()) pending.push_back(child.get());
        }
        doc_->set_layout_budget(kLayoutBudgetBase + kLayoutBudgetPerElement * elements);
        double t0 = now_ms();
        uint64_t hits = litehtml::document::s_layout_hits, misses = litehtml::document::s_layout_misses;
        doc_->render(litehtml::pixel_t(width));
        r_.stats_.layout_ms += now_ms() - t0;
        r_.stats_.layout_hits += litehtml::document::s_layout_hits - hits;
        r_.stats_.layout_misses += litehtml::document::s_layout_misses - misses;
        r_.stats_.positioned_ms += doc_->last_positioned_ms();
        if (doc_->layout_exhausted()) r_.stop_layout(doc_->layout_spent());
    }

    /// The last layout ran past the budget: the document is laid out no more.
    bool exhausted() const { return doc_ && doc_->layout_exhausted(); }
    /// Drops the document: the next build parses a new one.
    void drop() {
        elements_.clear();
        doc_ = nullptr;
    }

    /// The viewport is now `vp_height` tall, without a layout: the document does not depend on it.
    void viewport_resized(int vp_height) { vp_height_ = vp_height; }
    const std::string& master() const { return master_css_; }
    /// The master CSS of the next document. A change drops the current one: a patch keeps the master it was parsed with.
    void set_master(std::string css) {
        if (css == master_css_) return;
        master_css_ = std::move(css);
        elements_.clear();
        doc_ = nullptr;
    }
    /// False when the document is exactly as it was: same HTML, viewport and width.
    bool build(const std::string& html, int vp_height) {
        bool viewport_changed = vp_height != vp_height_;
        vp_height_ = vp_height;
        patched_ = false;
#ifdef TW_LITEHTML_TWIDGET
        // The fork folds viewport lengths to px as it parses, so a resize needs a fresh parse of a
        // document that has one. Media queries need no such care: a patched document re-evaluates them.
        bool viewport_units = litehtml::has_viewport_units(html);
        bool stale = (viewport_changed || float(r_.width_) != rendered_width_) && (viewport_units || viewport_units_);
        viewport_units_ = viewport_units;
#endif
#ifdef TW_LITEHTML_TWIDGET
        // twidget's litehtml fork patches a document made by createUpdatableFromString in place when the
        // new HTML only changes what it can patch (styles, text, classes, attributes, child lists), and
        // refuses otherwise, leaving the document as it was.
        bool changed = true;
        if (doc_ && !stale && doc_->update_from_string(html, &changed)) {
            patched_ = true;
            if (viewport_changed) doc_->media_changed();
            float width = float(r_.width_);
            if (!changed && !viewport_changed && width == rendered_width_) return false;
            if (!doc_->last_patch_paint_only() || viewport_changed || width != rendered_width_) {
                layout(width);
                rendered_width_ = width;
                layouts_++;
            }
            // A child-list patch replaces elements; a paint-only one cannot.
            if (!doc_->last_patch_paint_only()) index();
            return true;
        }
#endif
        elements_.clear();
        doc_ = nullptr;
#ifdef TW_LITEHTML_TWIDGET
        scans_.clear();
        // The fork's keys are only good within a document: a new one may resolve colours differently.
        cached_.clear();
        cached_commands_ = 0;
        doc_ = litehtml::document::createUpdatableFromString(html, this, master_css_);
#else
        doc_ = litehtml::document::createFromString(litehtml::estring(html), this, master_css_);
#endif
        if (!doc_) return true;
        rendered_width_ = float(r_.width_);
        layout(rendered_width_);
        layouts_++;
        index();
        return true;
    }

    void index() {
        elements_.clear();
        if (auto root = doc_->root()) {
            for (auto& el : root->select_all("[data-twid]")) {
                if (const char* a = el->get_attr("data-twid")) elements_[std::strtoll(a, nullptr, 10)] = el;
            }
        }
    }
    double height() const { return doc_ ? double(doc_->height()) : 0.0; }
#ifdef TW_LITEHTML_TWIDGET
    /// The widget id of an element, or of the ::before / ::after of one; 0 for none.
    static int64_t widget_id(const std::shared_ptr<litehtml::html_tag>& tag) {
        const char* a = tag->get_attr("data-twid");
        if (a) return std::strtoll(a, nullptr, 10);
        bool pseudo = (tag->tag() == litehtml::__tag_before_ || tag->tag() == litehtml::__tag_after_) && tag->parent();
        const char* owner = pseudo ? tag->parent()->get_attr("data-twid") : nullptr;
        return owner ? pseudo_id(std::strtoll(owner, nullptr, 10), tag->tag() == litehtml::__tag_after_) : 0;
    }
    /// What `tag` declares for `names` after the cascade. `inherit` is the parent's declaration (an
    /// `animation-delay: inherit` waits as long as its parent's); an SVG shape's presentation attribute (d,
    /// stroke-dashoffset, ...) is its value where no style sets one, as it is in a browser's cascade.
    static Declared declared_values(const std::shared_ptr<litehtml::html_tag>& tag, const std::vector<std::string>& names) {
        static const std::set<std::string> presentation = {"d", "stroke-dasharray", "stroke-dashoffset", "stroke-width", "fill", "stroke"};
        Declared found;
        for (const std::string& name : names) {
            litehtml::string_id id = raw_id(name);
            std::string raw = tag->raw_declaration_of(id);
            for (auto up = tag; css::iequals(raw, "inherit") && up;) {
                up = std::dynamic_pointer_cast<litehtml::html_tag>(up->parent());
                raw = up ? up->raw_declaration_of(id) : std::string();
            }
            if (raw.empty() && presentation.count(name))
                if (const char* attr = tag->get_attr(name.c_str()); attr && *attr && Container::svg_inside_drawing(tag)) raw = attr;
            if (!raw.empty()) found.emplace(name, std::move(raw));
        }
        return found;
    }
    /// The raw declaration id of a property, looked up once (a lookup of a name takes the string table's lock).
    static litehtml::string_id raw_id(const std::string& name) {
        thread_local std::unordered_map<std::string, litehtml::string_id> ids;
        auto it = ids.find(name);
        if (it == ids.end()) it = ids.emplace(name, litehtml::raw_declaration_id(litehtml::_id(name))).first;
        return it->second;
    }
    /// An element of an SVG drawing below its <svg>.
    static bool svg_inside_drawing(const std::shared_ptr<litehtml::html_tag>& tag) {
        for (auto p = tag->parent(); p; p = p->parent())
            if (std::dynamic_pointer_cast<litehtml::el_svg>(p)) return true;
        return false;
    }
    /// The declared values of `names` on every displayed element with a widget id that declares
    /// one of `gates`, or whose id is in `always`. What the fork records as raw declarations.
    /// `generation` tells one answer from another: the same number, the same declarations.
    const std::unordered_map<int64_t, Declared>& declarations(const std::vector<std::string>& names, const std::vector<std::string>& gates, const std::vector<int64_t>& always,
                                                              uint64_t* generation = nullptr) const {
        static const std::unordered_map<int64_t, Declared> nothing;
        if (generation) *generation = 0;
        if (!doc_ || !doc_->root()) return nothing;
        // A frame asks the same few questions of a document whose styles have not changed: the walk
        // is done once per style epoch for each.
        for (size_t i = 0; i < scans_.size(); i++) {
            const Scan& scan = scans_[i];
            if (scan.epoch != doc_->style_epoch() || scan.names != names || scan.gates != gates || scan.always != always) continue;
            std::rotate(scans_.begin(), scans_.begin() + i, scans_.begin() + i + 1);
            if (generation) *generation = scans_.front().generation;
            return scans_.front().result;
        }
        std::unordered_map<int64_t, Declared> out;
        std::function<void(const litehtml::element::ptr&)> visit = [&](const litehtml::element::ptr& el) {
            if (el->css().get_display() == litehtml::display_none) return;
            if (auto tag = std::dynamic_pointer_cast<litehtml::html_tag>(el)) {
                if (int64_t id = widget_id(tag)) {
                    bool listed = std::find(always.begin(), always.end(), id) != always.end();
                    bool open = listed;
                    for (size_t i = 0; !open && i < gates.size(); i++) open = !tag->raw_declaration_of(raw_id(gates[i])).empty();
                    if (open) {
                        Declared found = declared_values(tag, names);
                        if (!found.empty() || listed) out[id] = std::move(found);
                    }
                }
            }
            for (const auto& child : el->children()) visit(child);
        };
        visit(doc_->root());
        scans_.insert(scans_.begin(), Scan{names, gates, always, doc_->style_epoch(), ++scan_generation_, std::move(out)});
        if (scans_.size() > 4) scans_.pop_back();
        if (generation) *generation = scans_.front().generation;
        return scans_.front().result;
    }
    /// The values of `names` of every displayed element with a widget id, as the document has them now (none under
    /// display: none, as `declarations` reads none there); with `roots`, only those elements and their descendants.
    /// Taken before a change, they are what an element had before it (its before-change style, CSS Transitions §3).
    std::unordered_map<int64_t, Declared> snapshot(const std::vector<std::string>& names, const std::vector<int64_t>& alone,
                                                   const std::vector<int64_t>& subtrees, const std::function<bool(int64_t)>& skip) const {
        std::unordered_map<int64_t, Declared> out;
        if (!doc_ || !doc_->root()) return out;
        auto shown = [](litehtml::element::ptr el) {
            for (auto p = el; p; p = p->parent())
                if (p->css().get_display() == litehtml::display_none) return false;
            return el != nullptr;
        };
        auto read = [&](const litehtml::element::ptr& el) {
            if (auto tag = std::dynamic_pointer_cast<litehtml::html_tag>(el))
                if (int64_t id = widget_id(tag); id && !out.count(id) && !skip(id)) {
                    Declared found = declared_values(tag, names);
                    if (!found.empty()) out[id] = std::move(found);
                }
        };
        std::function<void(const litehtml::element::ptr&)> visit = [&](const litehtml::element::ptr& el) {
            if (el->css().get_display() == litehtml::display_none) return;
            read(el);
            for (const auto& child : el->children()) visit(child);
        };
        for (int64_t id : alone)
            if (auto el = element(id); shown(el)) read(el);
        for (int64_t id : subtrees)
            if (auto el = element(id); shown(el)) visit(el);
        return out;
    }
    /// `value` with each var(--name) replaced by element `id`'s custom property (or the fallback after the comma), as the
    /// cascade substitutes them: for diagnostics, so an exported animated value reads as numbers.
    std::string resolve_vars(int64_t id, const std::string& value) const {
        if (value.find("var(") == std::string::npos) return value;
        auto tag = std::dynamic_pointer_cast<litehtml::html_tag>(element(id));
        if (!tag) return value;
        std::string out;
        size_t pos = 0;
        for (size_t at; (at = value.find("var(", pos)) != std::string::npos;) {
            size_t close = at + 4;
            for (int depth = 1; close < value.size(); close++) {
                if (value[close] == '(') depth++;
                else if (value[close] == ')' && --depth == 0) break;
            }
            if (close >= value.size()) break;
            std::string inner = value.substr(at + 4, close - at - 4);
            size_t comma = inner.find(',');
            std::string name = css::trim(inner.substr(0, comma));
            litehtml::css_token_vector tokens;
            std::string replacement = comma == std::string::npos ? std::string() : css::trim(inner.substr(comma + 1));
            if (tag->get_custom_property(litehtml::_id(name), tokens) && !tokens.empty()) replacement = css::trim(litehtml::get_repr(tokens, 0, -1, true));
            out += value.substr(pos, at - pos) + replacement;
            pos = close + 1;
        }
        return out + value.substr(pos);
    }
    /// The colour element `id` inherits (its parent's computed colour), as `rgba(r, g, b, a)`.
    std::optional<std::string> parent_color(int64_t id) const {
        auto el = id > 0 ? element(id) : nullptr;
        auto parent = el ? el->parent() : nullptr;
        if (!parent) return std::nullopt;
        litehtml::web_color c = parent->css().get_color();
        return "rgba(" + std::to_string(c.red) + ", " + std::to_string(c.green) + ", " + std::to_string(c.blue) + ", " + css::format_number(c.alpha / 255.0) + ")";
    }
    /// Element `id` is in the document and displayed (no display: none on it or above it).
    bool displayed(int64_t id) const {
        auto el = id > 0 ? element(id) : nullptr;
        for (auto p = el; p; p = p->parent())
            if (p->css().get_display() == litehtml::display_none) return false;
        return el != nullptr;
    }
    /// Where and in what font element `id` shows its text, for a platform's editor.
    bool editor_style(int64_t id, Renderer::EditorSpec& spec) {
        auto el = element(id);
        auto ri = el ? el->get_render_item() : nullptr;
        if (!ri || !ri->is_visible()) return false;
        spec.frame = rect_of(el->get_placement());
        const auto& css = el->css();
        spec.font_family = css.font_family();
        spec.font_size = float(css.get_font_size());
        spec.font_weight = int(css.font_weight().val());
        spec.italic = css.font_style_value() == litehtml::font_style_italic;
        spec.color = color_of(css.get_color());
        auto align = css.get_text_align();
        spec.align = align == litehtml::text_align_center ? 1 : align == litehtml::text_align_right ? 2 : 0;
        auto font = fonts_.find(css.get_font());
        spec.font = font == fonts_.end() ? 0 : font->second.handle;
        Radii radii;
        if (!clip_of(el, spec.has_clip, spec.clip, radii)) spec.has_clip = false;
        return true;
    }
    /// What `id` declares for `names` after the cascade, its style attribute included.
    Declared declared(int64_t id, const std::vector<std::string>& names) const {
        Declared out;
        auto tag = std::dynamic_pointer_cast<litehtml::html_tag>(element(id));
        if (!tag) return out;
        for (const std::string& name : names) {
            std::string raw = tag->raw_declaration(name);
            if (!raw.empty()) out.emplace(name, std::move(raw));
        }
        return out;
    }
    /// Equal keys draw equal lists.
    std::tuple<const void*, uint32_t, uint64_t> paint_key() const { return {doc_.get(), doc_ ? doc_->paint_generation() : 0, layouts_}; }
    /// Whether the layout can change with the viewport's height, which follows the document's: fixed
    /// boxes, percentage heights on html or body, viewport lengths, height media queries, and absolute boxes
    /// placed against the viewport. Otherwise a new height needs no second layout.
    bool depends_on_viewport_height(const std::string& html) const {
        if (!doc_ || !doc_->root()) return false;
        if (viewport_units_) return true;
        // Width queries do not care (the theme's rewrite makes every widget have some): only those
        // that read the height, its ratio to the width or the orientation.
        for (size_t at = html.find("@media"); at != std::string::npos; at = html.find("@media", at + 6)) {
            std::string query = html.substr(at, html.find('{', at) - at);
            if (query.find("height") != std::string::npos || query.find("aspect") != std::string::npos || query.find("orientation") != std::string::npos) return true;
        }
        litehtml::position::vector fixed;
        doc_->get_fixed_boxes(fixed);
        if (!fixed.empty()) return true;
        auto percent = [](const litehtml::element::ptr& el) {
            return el->css().get_height().units() == litehtml::css_units_percentage || el->css().get_min_height().units() == litehtml::css_units_percentage;
        };
        auto root = doc_->root();
        if (percent(root)) return true;
        for (const auto& child : root->children())
            if (child->tag() == litehtml::_body_ && percent(child)) return true;
        for (const auto& [id, el] : elements_) {
            if (el->css().get_position() != litehtml::element_position_absolute) continue;
            bool contained = false;
            for (auto p = el->parent(); p && !contained; p = p->parent())
                contained = p->css().get_position() != litehtml::element_position_static;
            if (!contained) return true;
        }
        return false;
    }
    /// Lets go of what is made again on demand: fonts no document holds, measured widths, the paint
    /// trace and what was worked out from it, the declaration scans.
    void trim() {
        for (auto it = faces_.begin(); it != faces_.end();) {
            if (it->second->uses == 0) {
                backend_.delete_font(it->second->handle);
                it = faces_.erase(it);
            } else {
                it->second->forget();
                ++it;
            }
        }
        trace_.clear();
        trace_.shrink_to_fit();
        trace_key_ = {};
        cover_.clear();
        cover_.shrink_to_fit();
        cover_index_.clear();
        cover_trace_ = nullptr;
#ifdef TW_LITEHTML_TWIDGET
        scans_.clear();
        cached_.clear();
        cached_commands_ = 0;
#endif
    }
    /// The document was laid out at the current width and viewport: a patch can go straight in.
    bool can_patch() const { return doc_ && float(r_.width_) == rendered_width_; }
    /// Attribute values and sole texts into the document in place (the fork's holes), relaying out
    /// only when a value can move boxes. False, leaving the document as it was, when a change has no hole.
    bool patch(const std::vector<litehtml::document::hole_change>& changes) {
        bool changed = false;
        double t0 = now_ms();
        bool ok = doc_->update_holes(changes, &changed);
        r_.stats_.restyle_ms += now_ms() - t0;
        if (!ok) return false;
        patched_ = true;
        if (changed && !doc_->last_patch_paint_only()) {
            layout(rendered_width_);
            layouts_++;
        }
        return true;
    }
    /// Replaces the style attribute of elements in place (the fork's style holes), relaying out
    /// only when a value can move boxes. False when the document cannot patch them.
    bool restyle(const std::vector<std::pair<litehtml::element::ptr, std::string>>& patches) {
        if (!doc_) return false;
        bool changed = false;
        double t0 = now_ms();
        bool ok = doc_->update_styles(patches, &changed);
        r_.stats_.restyle_ms += now_ms() - t0;
        if (!ok) return false;
        patched_ = true;
        if (changed && !doc_->last_patch_paint_only()) {
            rendered_width_ = float(r_.width_);
            layout(rendered_width_);
            layouts_++;
        }
        return true;
    }

    /// Paint values (transform, opacity, background colour) of a ::before / ::after box in place: no restyle,
    /// no layout. False when the box is gone or the values are others (the caller builds).
    bool patch_generated(int64_t id, const std::string& declarations) {
        auto el = doc_ ? element(id) : nullptr;
        return el && doc_->patch_generated_style(el, declarations);
    }

    // compositor layers

    /// Counts layouts: boxes, and so what can cover a layer, change only with one.
    uint64_t layouts() const { return layouts_; }
    /// `el` can be painted on its own as a layer: a box with no effects of its own, under no
    /// ancestor that groups its paint. `box` gets its border box.
    /// `movers` are ancestors that may be paint groups: layers this one would move with.
    bool layer_candidate(const litehtml::element::ptr& el, Rect& box, const std::unordered_set<const litehtml::element*>* movers = nullptr) const {
        auto ri = el->get_render_item();
        if (!doc_ || !ri || !ri->is_visible() || (el->is_inline() && !el->is_inline_box() && !out_of_flow(el))) return false;
        const auto& css = el->css();
        // An own filter is painted into the layer's content (draw_layer); a backdrop needs what is under it.
        if (!css.get_backdrop_filter().empty()) return false;
        for (auto p = ri->parent(); p; p = p->parent())
            if (p->src_el()->css().is_paint_group() && !(movers && movers->count(p->src_el().get()))) return false;
        box = border_box_of(el);
        return box.w > 0 && box.h > 0 && box.w * box.h < 2'000'000;
    }
    /// Whether animating `left` / `top` only moves `el`: an absolutely positioned box whose right (bottom)
    /// is auto, so its size stays, out of the flow, so nothing else moves. `w` and `h` get the size of its
    /// containing block's padding box, what percentages there are of.
    bool offset_frame(const litehtml::element::ptr& el, bool left, bool top, float& w, float& h) const {
        const auto& css = el->css();
        if (css.get_position() != litehtml::element_position_absolute) return false;
        const auto& off = css.get_offsets();
        if ((left && !off.right.is_predefined()) || (top && !off.bottom.is_predefined())) return false;
        for (auto p = el->parent(); p; p = p->parent()) {
            if (p->css().get_position() == litehtml::element_position_static && !p->is_root()) continue;
            auto ri = p->get_render_item();
            if (!ri) return false;
            Rect b = border_box_of(p);
            const auto& bd = ri->get_borders();
            w = b.w - float(bd.left) - float(bd.right);
            h = b.h - float(bd.top) - float(bd.bottom);
            return w >= 0 && h >= 0;
        }
        return false;
    }
    /// Whether animating `el`'s height (or width) only stretches it about a fixed edge: a cross-axis
    /// height in a row flex container of a px height (the edge align-self / align-items says), or the
    /// size of an absolute box from its top (left) when its bottom (right) is auto. `anchor` gets where the
    /// fixed edge is (0 the top or left, .5 the centre, 1 the bottom or right), `ref` what percentages are of.
    bool size_frame(const litehtml::element::ptr& el, bool height, float& anchor, float& ref) const {
        const auto& css = el->css();
        auto parent = el->parent();
        if (!parent || !parent->get_render_item()) return false;
        if (css.get_position() == litehtml::element_position_absolute) {
            const auto& off = css.get_offsets();
            if (!(height ? off.bottom : off.right).is_predefined()) return false;
            float w, h;
            if (!offset_frame(el, false, false, w, h)) return false;
            anchor = 0;
            ref = height ? h : w;
            return true;
        }
        const auto& pc = parent->css();
        bool row = pc.get_display() == litehtml::display_flex || pc.get_display() == litehtml::display_inline_flex;
        row = row && (pc.get_flex_direction() == litehtml::flex_direction_row || pc.get_flex_direction() == litehtml::flex_direction_row_reverse);
        if (!height || !row || css.get_position() != litehtml::element_position_static) return false;
        // The container's height must not follow its items'.
        const litehtml::css_length& ph = pc.get_height();
        if (ph.is_predefined() || ph.units() != litehtml::css_units_px) return false;
        auto align = css.get_flex_align_self();
        if (align == litehtml::flex_align_items_auto) align = pc.get_flex_align_items();
        switch (int(align) & 0xff) {
        case litehtml::flex_align_items_flex_end: case litehtml::flex_align_items_end: anchor = 1; break;
        case litehtml::flex_align_items_center: anchor = 0.5f; break;
        case litehtml::flex_align_items_flex_start: case litehtml::flex_align_items_start: case litehtml::flex_align_items_normal:
        case litehtml::flex_align_items_stretch: anchor = 0; break;
        default: return false;
        }
        ref = float(parent->get_placement().height);
        return ref > 0;
    }
    /// A box that paints only a fill of its own (colour or gradient, corners of at most 2 px, no border,
    /// shadow, text or children): stretched, it looks as laid out at the new size, within a pixel.
    bool plain_fill(const litehtml::element::ptr& el) {
        if (!el->children().empty()) return false;
        Rect bounds;
        DisplayList list = draw_layer(el, {}, bounds);
        if (list.empty()) return false;
        for (const Command& c : list) {
            if (c.kind != Cmd::FillRect && c.kind != Cmd::Linear) return false;
            const Radii& q = c.radii;
            if (std::max({q.tlx, q.tly, q.trx, q.try_, q.brx, q.bry, q.blx, q.bly}) > 2) return false;
        }
        return true;
    }
    /// Absolutely positioned: a box of its own whatever its display says (CSS blockifies it; litehtml keeps
    /// a ::before's display inline).
    static bool out_of_flow(const litehtml::element::ptr& el) {
        auto p = el->css().get_position();
        return p == litehtml::element_position_absolute || p == litehtml::element_position_fixed;
    }
    /// Why layer_candidate refuses `el` (composition_debug).
    std::string candidate_why(const litehtml::element::ptr& el, const std::unordered_set<const litehtml::element*>* movers = nullptr) const {
        auto ri = el->get_render_item();
        if (!doc_ || !ri) return "not rendered";
        if (!ri->is_visible()) return "not visible";
        if (el->is_inline() && !el->is_inline_box() && !out_of_flow(el)) return "inline box";
        if (!el->css().get_backdrop_filter().empty()) return "has a backdrop-filter";
        for (auto p = ri->parent(); p; p = p->parent())
            if (p->src_el()->css().is_paint_group() && !(movers && movers->count(p->src_el().get()))) return "inside a paint group";
        return "size";
    }
    /// The ancestor that clips `el`, if one does: the innermost one's padding box and inner radii. A
    /// layer carries one clip, so false when an outer one could cut what the inner one lets through.
    bool clip_of(const litehtml::element::ptr& el, bool& has, Rect& rect, Radii& radii) const {
        has = false;
        auto ri = el->get_render_item();
        if (!ri) return false;
        for (auto p = ri->parent(); p; p = p->parent()) {
            if (!p->src_el()->css().clips_contents()) continue;
            Rect border = border_box_of(p->src_el());
            auto r = p->src_el()->css().get_borders().radius.calc_percents(litehtml::pixel_t(border.w), litehtml::pixel_t(border.h));
            if (has) {
                // Past its rounded corners an outer clip is a rectangle: it is moot when that holds the inner one.
                float corner = std::max({float(r.top_left_x), float(r.top_left_y), float(r.top_right_x), float(r.top_right_y),
                                         float(r.bottom_left_x), float(r.bottom_left_y), float(r.bottom_right_x), float(r.bottom_right_y)});
                Rect outer = padding_box_of(p.get());
                if (rect.x < outer.x + corner || rect.y < outer.y + corner || rect.x + rect.w > outer.x + outer.w - corner || rect.y + rect.h > outer.y + outer.h - corner)
                    return false;
                continue;
            }
            has = true;
            const auto& b = p->get_borders();
            rect = padding_box_of(p.get());
            radii = Radii{std::max(0.0f, float(r.top_left_x) - float(b.left)), std::max(0.0f, float(r.top_left_y) - float(b.top)),
                          std::max(0.0f, float(r.top_right_x) - float(b.right)), std::max(0.0f, float(r.top_right_y) - float(b.top)),
                          std::max(0.0f, float(r.bottom_right_x) - float(b.right)), std::max(0.0f, float(r.bottom_right_y) - float(b.bottom)),
                          std::max(0.0f, float(r.bottom_left_x) - float(b.left)), std::max(0.0f, float(r.bottom_left_y) - float(b.bottom))};
        }
        return true;
    }
    /// An animated background-color on `id` repaints `id` alone: no child inherits it, and nothing it
    /// holds samples what is behind it. Its descendants can still be layers.
    bool background_is_local(int64_t id) const {
        auto tag = std::dynamic_pointer_cast<litehtml::html_tag>(element(id));
        if (!tag || !tag->background_color_is_local()) return false;
        switch (tag->tag()) {
        case litehtml::_input_: case litehtml::_select_: case litehtml::_textarea_: case litehtml::_button_:
        case litehtml::_meter_: case litehtml::_progress_: case litehtml::_fieldset_:
            return false;
        default:
            break;
        }
        std::vector<const litehtml::element*> pending{tag.get()};
        while (!pending.empty()) {
            const litehtml::element* el = pending.back();
            pending.pop_back();
            if (!el->css().get_backdrop_filter().empty()) return false;
            for (const auto& child : el->children()) pending.push_back(child.get());
        }
        return true;
    }
    /// Render items in the order a full paint draws them. The draw callbacks return at once while
    /// tracing, and the trace holds until the document lays out or its paint changes.
    const std::vector<const litehtml::render_item*>& paint_order() {
        if (!doc_) {
            trace_.clear();
            return trace_;
        }
        auto key = std::make_tuple(doc_.get(), doc_->paint_generation(), layouts_);
        if (key == trace_key_) return trace_;
        trace_.clear();
        litehtml::g_paint_trace = &trace_;
        draw();
        litehtml::g_paint_trace = nullptr;
        trace_key_ = key;
        return trace_;
    }
    /// Something painted after `el` meets `area`, other than `el`'s descendants and what `ignore`
    /// holds (other layers, ordered among themselves by paint order). What shows of an item is its
    /// border box inside its clipping ancestors; hidden and fully transparent items show nothing.
    bool covered(const litehtml::element::ptr& el, const Rect& area, const std::vector<const litehtml::render_item*>& trace,
                 const std::unordered_set<const litehtml::element*>& ignore) {
        return scan(el, area, trace, ignore, nullptr, 0) != 0;
    }
    /// The widget elements painted after `el` that meet `area`, as `covered` finds them; nullopt when
    /// there are more than `limit` or one has no widget id.
    std::optional<std::vector<int64_t>> obscurers(const litehtml::element::ptr& el, const Rect& area, const std::vector<const litehtml::render_item*>& trace,
                                                  const std::unordered_set<const litehtml::element*>& ignore, size_t limit) {
        std::vector<int64_t> out;
        if (scan(el, area, trace, ignore, &out, limit) < 0) return std::nullopt;
        return out;
    }

    /// The items painted after `el` that meet `area`, skipping its descendants, what `ignore` holds and
    /// what shows nothing: 1 at the first when `ids` is null, otherwise their widget ids into `ids`
    /// (-1 past `limit` or for one without an id). 0 when nothing covers it; 1 too when `el` is not painted.
    int scan(const litehtml::element::ptr& el, const Rect& area, const std::vector<const litehtml::render_item*>& trace,
             const std::unordered_set<const litehtml::element*>& ignore, std::vector<int64_t>* ids, size_t limit) {
        // What each traced item can show, worked out once per trace: its border box inside its
        // clipping ancestors, and whether it or an ancestor is hidden or fully transparent.
        if (cover_trace_ != &trace || cover_key_ != trace_key_) {
            cover_.clear();
            cover_index_.clear();
            cover_.reserve(trace.size());
            for (size_t i = 0; i < trace.size(); i++) {
                const litehtml::render_item* item = trace[i];
                Cover c{clipped_border_box(item), item->src_el()->is_space() || item->src_el()->css().get_visibility() != litehtml::visibility_visible};
                for (const litehtml::render_item* walk = item; walk && !c.invisible; walk = walk->parent().get()) c.invisible = walk->src_el()->css().get_opacity() <= 0;
                cover_.push_back(c);
                cover_index_.emplace(item, i);
            }
            cover_trace_ = &trace;
            cover_key_ = trace_key_;
        }
        auto ri = el->get_render_item();
        auto self = cover_index_.find(ri.get());
        if (self == cover_index_.end()) return ids ? -1 : 1;
        for (size_t i = self->second + 1; i < trace.size(); i++) {
            const Cover& c = cover_[i];
            // The box test first: most items are nowhere near the layer.
            const Rect& box = c.box;
            if (c.invisible || box.w <= 0 || box.h <= 0) continue;
            if (!(box.x < area.x + area.w && area.x < box.x + box.w && box.y < area.y + area.h && area.y < box.y + box.h)) continue;
            bool skip = false;
            for (const litehtml::render_item* walk = trace[i]; walk && !skip; walk = walk->parent().get())
                skip = walk == ri.get() || ignore.count(walk->src_el().get());
            if (skip) continue;
            // A ::before or ::after counts as itself (its pseudo id), so it can become a still layer of its own.
            const auto& src = trace[i]->src_el();
            int64_t id = 0;
            if ((src->tag() == litehtml::__tag_before_ || src->tag() == litehtml::__tag_after_) && src->parent() && src->parent()->get_attr("data-twid"))
                id = pseudo_id(std::strtoll(src->parent()->get_attr("data-twid"), nullptr, 10), src->tag() == litehtml::__tag_after_);
            else
                id = widget_id(src, nullptr);
            // A canvas nothing drew on yet (a confetti overlay waiting for a tap) shows nothing.
            if (id && trace[i]->src_el()->tag() == litehtml::_canvas_ && !r_.canvas_draws_.count(int(id))) continue;
            if (!ids) return 1;
            if (!id) return -1;
            if (std::find(ids->begin(), ids->end(), id) != ids->end()) continue;
            if (ids->size() == limit) return -1;
            ids->push_back(id);
        }
        return ids && !ids->empty() ? 1 : 0;
    }
    /// Paints the document without the elements in `skip`.
    /// The document without `skip` and without the background images of `backgrounds`.
    DisplayList draw(const std::vector<litehtml::element::ptr>& skip, const std::vector<litehtml::element::ptr>& backgrounds) {
        if (!doc_) return {};
        for (const auto& el : skip) doc_->skip_paint.insert(el.get());
        for (const auto& el : backgrounds) doc_->skip_background_images.insert(el.get());
        DisplayList list = draw();
        doc_->skip_paint.clear();
        doc_->skip_background_images.clear();
        return list;
    }
    /// A shimmer's tile (the Swift renderer's backgroundTile): `el`'s background is one gradient, not
    /// repeated, on a box without children or borders, which its position can slide under the clip
    /// without changing anything else. `content` gets the tile drawn whole where it is.
    bool background_tile(const litehtml::element::ptr& el, Rect& tile, Rect& area, Rect& clip, Radii& radii, DisplayList& content) {
        auto ri = el->get_render_item();
        if (!doc_ || !ri || el->is_root() || el->is_inline() || !el->children().empty() || el->css().is_paint_group()) return false;
        // The layer goes over the document, and would cover the borders the background lies under.
        if (el->css().get_borders().is_visible()) return false;
        const litehtml::background* bg = el->get_background();
        if (!bg || bg->m_image.size() != 1 || bg->m_repeat.empty() || bg->m_repeat[0] != litehtml::background_repeat_no_repeat) return false;
        auto type = bg->get_layer_type(0);
        if (type != litehtml::background::type_linear_gradient && type != litehtml::background::type_radial_gradient &&
            type != litehtml::background::type_conic_gradient)
            return false;
        if (!bg->m_origin.empty() && bg->m_origin[0] != litehtml::background_box_padding) return false;
        if (!bg->m_attachment.empty() && bg->m_attachment[0] != litehtml::background_attachment_scroll) return false;
        for (auto p = ri->parent(); p; p = p->parent()) {
            const auto& css = p->src_el()->css();
            if (css.is_paint_group() || css.get_position() == litehtml::element_position_fixed) return false;
        }
        litehtml::position box = ri->get_placement();
        litehtml::background_layer layer;
        if (!bg->get_layer(0, box, el.get(), ri, layer)) return false;
        tile = rect_of(layer.origin_box);
        clip = rect_of(layer.clip_box);
        radii = layer_radii(layer);
        litehtml::position padding = box;
        padding += ri->get_paddings();
        area = rect_of(padding);
        if (tile.empty() || clip.empty()) return false;
        litehtml::background_layer whole = layer;
        whole.clip_box = layer.origin_box;
        whole.border_box = layer.origin_box;
        whole.border_radius = litehtml::border_radiuses();
        list_.clear();
        groups_.clear();
        clips_.clear();
        suspended_.clear();
        bg->draw_layer(0, 0, whole, this);
        content = std::move(list_);
        return !content.empty();
    }
    /// The outermost <svg> an element of an SVG drawing is in, or null for an HTML element.
    static std::shared_ptr<litehtml::el_svg> svg_root_of(const litehtml::element::ptr& el) {
        for (auto p = el->parent(); p; p = p->parent())
            if (auto svg = std::dynamic_pointer_cast<litehtml::el_svg>(p)) {
                for (auto up = svg->parent(); up; up = up->parent())
                    if (auto outer = std::dynamic_pointer_cast<litehtml::el_svg>(up)) svg = outer;
                return svg;
            }
        return nullptr;
    }
    /// An element of an SVG drawing alone, as its <svg> would draw it, without its own transform and opacity.
    DisplayList draw_svg_alone(const litehtml::element::ptr& el, const std::vector<litehtml::element::ptr>* skip = nullptr) {
        list_.clear();
        groups_.clear();
        clips_.clear();
        suspended_.clear();
        auto root = svg_root_of(el);
        auto ri = root ? root->get_render_item() : nullptr;
        if (!doc_ || !ri || !ri->is_visible()) return {};
        if (skip)
            for (const auto& other : *skip)
                if (other != el) doc_->skip_paint.insert(other.get());
        doc_->svg_only = el.get();
        litehtml::position pos = ri->get_placement();
        root->paint_shapes(0, pos);
        doc_->svg_only = nullptr;
        doc_->skip_paint.clear();
        return std::move(list_);
    }
    /// Whether an element of an SVG drawing draws anything at all (an invisible stroke, a zero opacity draw nothing).
    bool svg_draws_something(const litehtml::element::ptr& el) {
        DisplayList list = draw_svg_alone(el);
        for (const Command& c : list) {
            switch (c.kind) {
            case Cmd::ClipPush: case Cmd::ClipPop: case Cmd::GroupPush: case Cmd::GroupPop: case Cmd::FilterPush: case Cmd::FilterPop: break;
            case Cmd::Path: {
                if (!c.path) break;
                // One straight segment (a <line>) has no area to fill.
                bool segment = c.path->points.size() <= 4 &&
                               std::all_of(c.path->ops.begin(), c.path->ops.end(), [](Path::Op op) { return op == Path::Move || op == Path::Line; });
                if ((c.path->fill && !segment && (c.path->fill_color.a > 0 || c.path->gradient)) || (c.path->stroke && (c.path->stroke_color.a > 0 || c.path->stroke_gradient)))
                    return true;
                break;
            }
            default:
                if (c.kind != Cmd::FillRect || c.color.a > 0) return true;
            }
        }
        return false;
    }
    /// `el` and its descendants alone, without `el`'s own transform, opacity and stacking group
    /// (the layer applies them) and without the other layers in `skip`. `bounds` gets the area the
    /// paint can reach.
    DisplayList draw_layer(const litehtml::element::ptr& el, const std::vector<litehtml::element::ptr>& skip, Rect& bounds) {
        if (auto root = svg_root_of(el)) {
            // An element of an SVG drawing: within its <svg>'s box, which clips the layer (Promotion::clip).
            DisplayList list = draw_svg_alone(el, &skip);
            Rect box = border_box_of(root);
            bounds = Rect{std::floor(box.x - LAYER_MARGIN), std::floor(box.y - LAYER_MARGIN), std::ceil(box.w + 2 * LAYER_MARGIN), std::ceil(box.h + 2 * LAYER_MARGIN)};
            return list;
        }
        list_.clear();
        groups_.clear();
        clips_.clear();
        suspended_.clear();
        auto ri = el->get_render_item();
        if (!doc_ || !ri) return {};
        for (const auto& other : skip)
            if (other != el) doc_->skip_paint.insert(other.get());
        litehtml::position placement = ri->get_placement();
        litehtml::pixel_t x = placement.x - ri->pos().x, y = placement.y - ri->pos().y;
        // The element's own filter stays in its content: CSS filters an element in its local space and
        // transforms the result, which is what the layer's matrix does to the filtered content.
        const auto& filter = el->css().get_filter();
        float reach = 0;
        auto paint = [&](litehtml::uint_ptr hdc) {
            el->draw(hdc, x, y, nullptr, ri);
            if (ri->is_visible()) draw_stacking_context(ri, x, y);
        };
        if (!filter.empty()) {
            Rect b = border_box_of(el);
            litehtml::filter_paint f;
            f.steps = &filter;
            f.owner = el.get();
            f.box = litehtml::position(litehtml::pixel_t(b.x), litehtml::pixel_t(b.y), litehtml::pixel_t(b.w), litehtml::pixel_t(b.h));
            f.bounds = f.box;
            const auto& ink = ri->ink(doc_->paint_generation(), false);
            if (!ink.empty && !ink.unbounded)
                f.bounds = litehtml::position(litehtml::pixel_t(ink.l) + x, litehtml::pixel_t(ink.t) + y, litehtml::pixel_t(ink.r - ink.l), litehtml::pixel_t(ink.b - ink.t));
            reach = litehtml::filter_reach(filter);
            draw_filtered(0, f, paint);
        } else {
            paint(0);
        }
        doc_->skip_paint.clear();
        Rect box = border_box_of(el);
        litehtml::position redraw(litehtml::pixel_t(box.x), litehtml::pixel_t(box.y), litehtml::pixel_t(box.w), litehtml::pixel_t(box.h));
        ri->get_redraw_box(redraw, x, y);
        // The redraw box leaves out shadows and anti-aliasing: a margin keeps them.
        float x0 = std::min(box.x, float(redraw.x)) - LAYER_MARGIN - reach, y0 = std::min(box.y, float(redraw.y)) - LAYER_MARGIN - reach;
        float x1 = std::max(box.x + box.w, float(redraw.x) + float(redraw.width)) + LAYER_MARGIN + reach;
        float y1 = std::max(box.y + box.h, float(redraw.y) + float(redraw.height)) + LAYER_MARGIN + reach;
        bounds = Rect{std::floor(x0), std::floor(y0), std::ceil(x1) - std::floor(x0), std::ceil(y1) - std::floor(y0)};
        return std::move(list_);
    }
#endif
    /// The pointer left the view: :hover ends. True when something must repaint.
    bool leave() {
        cursor_.clear();
        if (!doc_) return false;
        bool redraw = false;
        doc_->on_mouse_leave([&](const litehtml::position&) { redraw = true; });
        return redraw;
    }
    /// A decoded image for the canvas's drawImage: loads `url` like an <img> would.
    bool image_of(const std::string& url, int& id, float& w, float& h) {
        load_image(url.c_str(), nullptr, false);
        auto it = images_.find(url);
        if (it == images_.end() || (!it->second.id && !it->second.placeholder)) return false;
        id = it->second.id;
        w = it->second.w;
        h = it->second.h;
        return true;
    }
    /// An asynchronously decoded image got its size; true when a known image changed.
    bool image_ready(int id, float w, float h) {
        for (auto& [url, img] : images_) {
            if (img.id == id) {
                if (img.w == w && img.h == h) return false;
                img.w = w;
                img.h = h;
                return true;
            }
        }
        return false;
    }
    litehtml::document* doc() const { return doc_.get(); }
    /// What a litehtml font handle is for the backend: its font, its descent and its spacings.
    bool font_info(uintptr_t h, int& handle, float& descent, float& letter, float& word) const {
        auto it = fonts_.find(litehtml::uint_ptr(h));
        if (it == fonts_.end()) return false;
        handle = it->second.handle;
        descent = it->second.metrics.descent;
        letter = it->second.letter_spacing;
        word = it->second.word_spacing;
        return true;
    }
    litehtml::element::ptr element(int64_t id) const {
        if (id > 0 && (id & RING_BIT)) return element(id & ~RING_BIT);
        if (id < 0) {
            auto owner = element(pseudo_owner(id));
            if (!owner || owner->children().empty()) return nullptr;
            auto el = pseudo_after(id) ? owner->children().back() : owner->children().front();
            return el->tag() == (pseudo_after(id) ? litehtml::__tag_after_ : litehtml::__tag_before_) ? el : nullptr;
        }
        auto it = elements_.find(id);
        return it == elements_.end() ? nullptr : it->second;
    }
    const std::unordered_map<int64_t, litehtml::element::ptr>& elements() const { return elements_; }
    /// The widget id of a litehtml element or of its nearest ancestor with one (0 when none).
    int64_t widget_id(litehtml::element::ptr el, Rect* placement) const {
        while (el) {
            if (const char* a = el->get_attr("data-twid")) {
                if (placement) *placement = border_box_of(el);
                return std::strtoll(a, nullptr, 10);
            }
            el = el->parent();
        }
        return 0;
    }
    /// Whether a hit may land on the item: `pointer-events: none` passes it on to what is below.
    static bool hittable(const std::shared_ptr<litehtml::render_item>& ri) {
        return computed_pointer_events(ri->src_el()) != "none";
    }
    litehtml::element::ptr element_at(double x, double y) const {
        if (!doc_ || !doc_->root_render()) return nullptr;
        auto px = litehtml::pixel_t(float(x)), py = litehtml::pixel_t(float(y));
        return doc_->root_render()->get_element_by_point(px, py, px, py, hittable);
    }
    /// What is at (x, y) in document coordinates, looking only inside `el`.
    litehtml::element::ptr element_at_within(const litehtml::element::ptr& el, double x, double y) const {
        auto ri = el ? el->get_render_item() : nullptr;
        if (!doc_ || !ri) return nullptr;
        // get_element_by_point takes the point relative to the item's parent content box.
        auto placement = ri->get_placement();
        auto sticky = ri->sticky_offset();
        auto px = litehtml::pixel_t(float(x)) - (placement.x - ri->pos().x - sticky.x);
        auto py = litehtml::pixel_t(float(y)) - (placement.y - ri->pos().y - sticky.y);
        return ri->get_element_by_point(px, py, litehtml::pixel_t(float(x)), litehtml::pixel_t(float(y)), hittable);
    }
    /// Pointer state for :hover / :active; true when something must repaint. A lifted finger leaves
    /// :hover; a mouse released over the view keeps it.
    bool mouse(int action, double x, double y, bool hovers = false) {
        if (!doc_) return false;
        bool redraw = false;
        auto box = [&](const litehtml::position&) { redraw = true; };
        auto px = litehtml::pixel_t(float(x)), py = litehtml::pixel_t(float(y));
        switch (action) {
        case 0: doc_->on_mouse_over(px, py, px, py, box); doc_->on_lbutton_down(px, py, px, py, box); break;
        case 1: doc_->on_mouse_over(px, py, px, py, box); break;
        case 2:
            doc_->on_lbutton_up(px, py, px, py, box);
            if (!hovers) doc_->on_mouse_leave(box);
            break;
        default: doc_->on_button_cancel(box); doc_->on_mouse_leave(box); break;
        }
        return redraw;
    }
    DisplayList draw() {
        list_.clear();
        clips_.clear();
        suspended_.clear();
#ifdef TW_LITEHTML_TWIDGET
        groups_.clear();
#endif
        if (doc_) {
            litehtml::position clip(0, 0, litehtml::pixel_t(float(r_.width_)), litehtml::pixel_t(float(vp_height_)));
            doc_->draw(0, 0, 0, &clip);
        }
        return std::move(list_);
    }
    // ── document_container: fonts and text ──
    litehtml::uint_ptr create_font(const litehtml::font_description& descr, const litehtml::document*, litehtml::font_metrics* fm) override {
        Face* face = face_for(descr.family, float(descr.size), descr.weight, descr.style == litehtml::font_style_italic, descr.features);
        if (!face) return 0;
        face->uses++;
        Font f;
        f.face = face;
        f.handle = face->handle;
        f.metrics = line_metrics(face->metrics, float(r_.dpr_));
        const FontMetrics& m = f.metrics;
        f.size = float(descr.size);
        f.decoration = descr.decoration_line;
        f.decoration_color = descr.decoration_color;
        f.letter_spacing = descr.letter_spacing;
        f.word_spacing = descr.word_spacing;
        if (fm) {
            fm->font_size = descr.size;
            fm->ascent = litehtml::pixel_t(m.ascent);
            fm->descent = litehtml::pixel_t(m.descent);
            fm->height = litehtml::pixel_t(std::max(m.height, m.ascent + m.descent));
            fm->x_height = litehtml::pixel_t(m.x_height > 0 ? m.x_height : m.ascent * 0.5f);
            fm->ch_width = litehtml::pixel_t(m.zero_width);
            fm->draw_spaces = descr.decoration_line != litehtml::text_decoration_line_none;
            fm->sub_shift = litehtml::pixel_t(f.size / 5);
            fm->super_shift = litehtml::pixel_t(f.size / 3);
            fm->device_pixel = litehtml::pixel_t(1.0f / std::max(1e-3f, float(r_.dpr_)));
            fm->em_ascent = litehtml::pixel_t(m.em_ascent);  // 0 when the backend cannot read it
        }
        litehtml::uint_ptr id = ++next_font_;
        fonts_[id] = f;
        return id;
    }
    void delete_font(litehtml::uint_ptr h) override {
        auto it = fonts_.find(h);
        if (it == fonts_.end()) return;
        // The face stays for the next document, which asks for the same fonts.
        it->second.face->uses--;
        fonts_.erase(it);
    }
#ifdef TW_LITEHTML_TWIDGET
    std::shared_ptr<litehtml::text_breaker> create_text_breaker(const std::string& text, litehtml::uint_ptr font) override {
        return fonts_.count(font) ? std::make_shared<TextBreaker>(*this, text, font) : nullptr;
    }
#endif
    litehtml::pixel_t text_width(const char* text, litehtml::uint_ptr h) override {
        auto it = fonts_.find(h);
        if (it == fonts_.end() || !text) return 0;
        return litehtml::pixel_t(it->second.width(backend_, text));
    }
    void draw_text(litehtml::uint_ptr, const char* text, litehtml::uint_ptr h, litehtml::web_color color, const litehtml::position& pos) override {
        if (litehtml::g_paint_trace) return;
        auto it = fonts_.find(h);
        if (it == fonts_.end() || !text || !*text) return;
        const Font& f = it->second;
        Command c;
        c.kind = Cmd::Text;
        c.font = f.handle;
        c.text = text;
        c.color = color_of(color);
        c.x = float(pos.x);
        // litehtml hands the line box; the baseline sits `descent` above its bottom (as the cairo container does).
        c.y = float(pos.y) + float(pos.height) - f.metrics.descent;
        // The line box, for damage: where the run can paint.
        c.rect = Rect{float(pos.x), float(pos.y), float(pos.width) > 0 ? float(pos.width) : f.width(backend_, text), float(pos.height)};
        if (f.letter_spacing != 0 || (f.word_spacing != 0 && spacing_spaces(text))) {
            // A painter draws a run as the font spaces it: each character goes on its own, or each word
            // when only the spaces widen, so that words keep their shaping.
            float x = c.x;
            for (std::string_view unit : f.letter_spacing != 0 ? spacing_units(text) : spacing_words(text)) {
                Command u = c;
                u.text = std::string(unit);
                u.x = x;
                u.rect.x = x;
                u.rect.w = f.width(backend_, unit);
                x += u.rect.w;
                if (!u.text.empty() && u.text != " " && u.text != "\xC2\xA0") list_.push_back(std::move(u));
            }
        } else {
            list_.push_back(std::move(c));
        }
        if (f.decoration != litehtml::text_decoration_line_none) {
            float w = float(pos.width);
            if (w <= 0) w = backend_.text_width(f.handle, text);
            float thickness = std::max(1.0f, f.size / 14.0f);
            Color dc = f.decoration_color.is_current_color ? color_of(color) : color_of(f.decoration_color);
            float baseline = float(pos.y) + float(pos.height) - f.metrics.descent;
            auto line = [&](float y) {
                Command l;
                l.kind = Cmd::FillRect;
                l.rect = Rect{float(pos.x), y, w, thickness};
                l.color = dc;
                list_.push_back(std::move(l));
            };
            if (f.decoration & litehtml::text_decoration_line_underline) line(baseline + std::max(1.0f, f.metrics.descent * 0.35f));
            if (f.decoration & litehtml::text_decoration_line_line_through) line(baseline - f.metrics.ascent * 0.3f - thickness / 2);
            if (f.decoration & litehtml::text_decoration_line_overline) line(float(pos.y));
        }
    }
    litehtml::pixel_t pt_to_px(float pt) const override { return litehtml::pixel_t(pt * 96.0f / 72.0f); }
    litehtml::pixel_t get_default_font_size() const override { return 16; }
    const char* get_default_font_name() const override { return "-apple-system, system-ui, sans-serif"; }
    void draw_list_marker(litehtml::uint_ptr, const litehtml::list_marker& marker) override {
        if (litehtml::g_paint_trace) return;
        Command c;
        c.rect = rect_of(marker.pos);
        c.color = color_of(marker.color);
        switch (marker.marker_type) {
        case litehtml::list_style_type_none: return;
        case litehtml::list_style_type_disc: c.kind = Cmd::FillEllipse; break;
        case litehtml::list_style_type_circle: c.kind = Cmd::StrokeEllipse; c.width = 1; break;
        case litehtml::list_style_type_square: c.kind = Cmd::FillRect; break;
        case litehtml::list_style_type_disclosure_open:
        case litehtml::list_style_type_disclosure_closed: {
            // The marker box is a third of the font size; the triangle takes half of it, in the 1em
            // an inside marker is given.
            float em = c.rect.w * 3, cx = c.rect.x + em * 0.3f, cy = c.rect.y + c.rect.h / 2;
            bool open = marker.marker_type == litehtml::list_style_type_disclosure_open;
            const float closed_points[] = {-0.2f, -0.25f, 0.25f, 0, -0.2f, 0.25f};
            const float open_points[] = {-0.25f, -0.2f, 0.25f, -0.2f, 0, 0.25f};
            const float* points = open ? open_points : closed_points;
            auto triangle = std::make_shared<Path>();
            for (int i = 0; i < 6; i += 2) {
                triangle->ops.push_back(i ? Path::Line : Path::Move);
                triangle->points.push_back(cx + points[i] * em);
                triangle->points.push_back(cy + points[i + 1] * em);
            }
            triangle->ops.push_back(Path::Close);
            triangle->fill = true;
            triangle->fill_color = c.color;
            c.kind = Cmd::Path;
            c.path = std::move(triangle);
            break;
        }
        default: {
            auto it = fonts_.find(marker.font);
            if (it == fonts_.end()) return;
            c.kind = Cmd::Text;
            c.font = it->second.handle;
            c.text = marker_text(marker.marker_type, marker.index);
            c.x = float(marker.pos.x);
            c.y = float(marker.pos.y) + float(marker.pos.height) - it->second.metrics.descent;
        }
        }
        list_.push_back(std::move(c));
    }

    // ── images: data: only, decoded by the backend, referenced by id ──
    void load_image(const char* src, const char*, bool) override {
        if (!src) return;
        std::string url = src;
        if (images_.count(url)) return;
        Image img;
        if (url.rfind("tw-canvas:", 0) == 0) {
            // The renderer's own source for a <canvas>: tw-canvas:<node id>/<w>x<h>, drawn from the backend's bitmap.
            int id = 0, w = 0, h = 0;
            if (std::sscanf(url.c_str() + 10, "%d/%dx%d", &id, &w, &h) == 3) {
                img.canvas = id;
                img.w = float(w);
                img.h = float(h);
            }
        } else if (std::string mime = data_mime(url); mime == "image/svg+xml") {
            // Profile 1 has no SVG images: the validator puts its 48 px placeholder, an SVG, in place of
            // every image it refuses. No SVG reaches a backend, whatever the spelling.
            img.placeholder = true;
            img.w = img.h = 48;
        } else if (std::find(std::begin(IMAGE_TYPES), std::end(IMAGE_TYPES), mime) != std::end(IMAGE_TYPES)) {
            // The backend may answer the size later (`image_ready`): the id stays, the size is 0 until then.
            img.id = ++next_image_;
            if (!backend_.load_image(img.id, url, img.w, img.h)) img.id = 0;
        } else {
            r_.pending_violations_.push_back("image refused: " + url.substr(0, 80));
        }
        images_[url] = img;
    }
    void get_image_size(const char* src, const char*, litehtml::size& sz) override {
        auto it = src ? images_.find(src) : images_.end();
        if (it == images_.end()) { sz.width = 0; sz.height = 0; return; }
        sz.width = litehtml::pixel_t(it->second.w);
        sz.height = litehtml::pixel_t(it->second.h);
    }
    void draw_image(litehtml::uint_ptr, const litehtml::background_layer& layer, const std::string& url, const std::string&) override {
        if (litehtml::g_paint_trace) return;
        auto it = images_.find(url);
        if (it == images_.end() || (!it->second.id && !it->second.canvas && !it->second.placeholder)) return;
        if (it->second.placeholder) {
            draw_placeholder(layer);
            return;
        }
        Command c;
        c.kind = Cmd::Image;
        c.image = it->second.id;
        c.canvas = it->second.canvas;
        c.rect = rect_of(layer.origin_box);
        c.radii = layer_radii(layer);
        c.clip = rect_of(layer.clip_box);
        c.has_clip = true;
        // the border box bounds the drawing too
        Rect b = rect_of(layer.border_box);
        clip_intersect(c.clip, b);
        // background-repeat: the tile is placed once by litehtml and repeated here over the clip, as one
        // command per tile, up to a bound past which a painter would only be slowed down.
        const bool across = layer.repeat == litehtml::background_repeat_repeat || layer.repeat == litehtml::background_repeat_repeat_x;
        const bool down = layer.repeat == litehtml::background_repeat_repeat || layer.repeat == litehtml::background_repeat_repeat_y;
        const Rect tile = c.rect;
        if ((across || down) && r_.image_tiling() && tile.w >= 1 && tile.h >= 1 && !c.clip.empty()) {
            c.repeat = uint8_t((across ? 1 : 0) | (down ? 2 : 0));
            list_.push_back(std::move(c));
            return;
        }
        if ((across || down) && tile.w >= 1 && tile.h >= 1 && !c.clip.empty()) {
            float x0 = across ? tile.x - std::ceil((tile.x - c.clip.x) / tile.w) * tile.w : tile.x;
            float y0 = down ? tile.y - std::ceil((tile.y - c.clip.y) / tile.h) * tile.h : tile.y;
            float x1 = across ? c.clip.x + c.clip.w : tile.x + tile.w, y1 = down ? c.clip.y + c.clip.h : tile.y + tile.h;
            if (std::ceil((x1 - x0) / tile.w) * std::ceil((y1 - y0) / tile.h) <= 4096) {
                for (float y = y0; y < y1; y += tile.h) {
                    for (float x = x0; x < x1; x += tile.w) {
                        Command t = c;
                        t.rect = Rect{x, y, tile.w, tile.h};
                        list_.push_back(std::move(t));
                    }
                }
                return;
            }
        }
        list_.push_back(std::move(c));
    }

    /// widget-sandbox's placeholder (url-policy.ts, PLACEHOLDER_IMAGE), its 48 × 48 drawing fitted into the image, centred.
    void draw_placeholder(const litehtml::background_layer& layer) {
        Rect to = rect_of(layer.origin_box);
        Rect clip = rect_of(layer.clip_box);
        clip_intersect(clip, rect_of(layer.border_box));
        float sx = to.w / 48, sy = to.h / 48;
        // The placeholder is an SVG with a view box, so it keeps its aspect ratio, centred
        // (preserveAspectRatio xMidYMid meet), as browsers and LHContainer draw it.
        sx = sy = std::min(sx, sy);
        to = Rect{to.x + (to.w - 48 * sx) / 2, to.y + (to.h - 48 * sy) / 2, 48 * sx, 48 * sy};
        const Color faint{136, 136, 136, 34}, mark{136, 136, 136, 102};
        Command c;
        c.kind = Cmd::ClipPush;
        c.rect = clip;
        list_.push_back(c);
        c = Command{};
        c.kind = Cmd::FillRect;
        c.rect = to;
        c.color = faint;
        list_.push_back(c);
        auto hills = std::make_shared<Path>();
        const float points[] = {8, 36, 18, 24, 26, 33, 32, 26, 40, 36};
        for (size_t i = 0; i < std::size(points); i += 2) {
            hills->ops.push_back(i ? Path::Line : Path::Move);
            hills->points.push_back(to.x + points[i] * sx);
            hills->points.push_back(to.y + points[i + 1] * sy);
        }
        hills->ops.push_back(Path::Close);
        hills->fill = true;
        hills->fill_color = mark;
        c = Command{};
        c.kind = Cmd::Path;
        c.path = std::move(hills);
        list_.push_back(c);
        c = Command{};
        c.kind = Cmd::FillEllipse;
        c.rect = Rect{to.x + 29 * sx, to.y + 11 * sy, 8 * sx, 8 * sy};
        c.color = mark;
        list_.push_back(c);
        c = Command{};
        c.kind = Cmd::ClipPop;
        list_.push_back(c);
    }

    // ── backgrounds ──
    void draw_solid_fill(litehtml::uint_ptr, const litehtml::background_layer& layer, const litehtml::web_color& color) override {
        if (litehtml::g_paint_trace) return;
        if (color.alpha == 0) return;
        Command c;
        c.kind = Cmd::FillRect;
        c.color = color_of(color);
        if (layer.is_root) {
            // the root background covers the whole canvas, like a browser's
            c.rect = Rect{0, 0, float(r_.width_), float(vp_height_)};
        } else {
            c.rect = layer_shape(layer);
            c.radii = layer_radii(layer);
            c.clip = rect_of(layer.clip_box);
            c.has_clip = true;
        }
        list_.push_back(std::move(c));
    }
    void gradient_common(Command& c, const litehtml::background_layer& layer, const litehtml::background_layer::gradient_base& g) {
        c.rect = layer_shape(layer);
        c.radii = layer_radii(layer);
        c.clip = rect_of(layer.clip_box);
        c.has_clip = true;
        for (const auto& p : g.color_points) c.stops.push_back(Stop{p.offset, color_of(p.color)});
    }
    /// A gradient layer is an image the size of its tile (background-size, placed by background-position: litehtml's
    /// origin_box): drawn in that tile only, and repeated over the clip as background-repeat says, one command per
    /// tile with the gradient moved along (`linear` moves its end point too). A tile that covers the whole clip, or
    /// more tiles than a painter should draw, is the one command as before.
    void push_gradient_tiles(Command c, const litehtml::background_layer& layer, bool linear) {
        const Rect tile = rect_of(layer.origin_box);
        Rect area = c.clip;
        clip_intersect(area, rect_of(layer.border_box));
        auto covers = [&](const Rect& t) { return t.x <= area.x && t.y <= area.y && t.x + t.w >= area.x + area.w && t.y + t.h >= area.y + area.h; };
        if (area.empty() || tile.w < 1 || tile.h < 1 || covers(tile)) {
            list_.push_back(std::move(c));
            return;
        }
        const bool across = layer.repeat == litehtml::background_repeat_repeat || layer.repeat == litehtml::background_repeat_repeat_x;
        const bool down = layer.repeat == litehtml::background_repeat_repeat || layer.repeat == litehtml::background_repeat_repeat_y;
        float x0 = across ? tile.x - std::ceil((tile.x - area.x) / tile.w) * tile.w : tile.x;
        float y0 = down ? tile.y - std::ceil((tile.y - area.y) / tile.h) * tile.h : tile.y;
        float x1 = across ? area.x + area.w : tile.x + tile.w, y1 = down ? area.y + area.h : tile.y + tile.h;
        if (std::ceil((x1 - x0) / tile.w) * std::ceil((y1 - y0) / tile.h) > 4096) {
            list_.push_back(std::move(c));
            return;
        }
        for (float y = y0; y < y1; y += tile.h) {
            for (float x = x0; x < x1; x += tile.w) {
                Command t = c;
                t.clip = area;
                clip_intersect(t.clip, Rect{x, y, tile.w, tile.h});
                if (t.clip.empty()) continue;
                const float dx = x - tile.x, dy = y - tile.y;
                t.ax += dx;
                t.ay += dy;
                if (linear) {
                    t.bx += dx;
                    t.by += dy;
                }
                list_.push_back(std::move(t));
            }
        }
    }
    void draw_linear_gradient(litehtml::uint_ptr, const litehtml::background_layer& layer, const litehtml::background_layer::linear_gradient& g) override {
        if (litehtml::g_paint_trace) return;
        Command c;
        c.kind = Cmd::Linear;
        gradient_common(c, layer, g);
        c.ax = g.start.x; c.ay = g.start.y; c.bx = g.end.x; c.by = g.end.y;
        push_gradient_tiles(std::move(c), layer, true);
    }
    void draw_radial_gradient(litehtml::uint_ptr, const litehtml::background_layer& layer, const litehtml::background_layer::radial_gradient& g) override {
        if (litehtml::g_paint_trace) return;
        Command c;
        c.kind = Cmd::Radial;
        gradient_common(c, layer, g);
        c.ax = g.position.x; c.ay = g.position.y; c.bx = g.radius.x; c.by = g.radius.y;
        push_gradient_tiles(std::move(c), layer, false);
    }
    void draw_conic_gradient(litehtml::uint_ptr, const litehtml::background_layer& layer, const litehtml::background_layer::conic_gradient& g) override {
        if (litehtml::g_paint_trace) return;
        Command c;
        c.kind = Cmd::Conic;
        gradient_common(c, layer, g);
        c.ax = g.position.x; c.ay = g.position.y; c.bx = g.angle; c.by = g.radius;
        push_gradient_tiles(std::move(c), layer, false);
    }

    // ── borders: one stroke when uniform, otherwise each side on its own; dashed, dotted and double by their style ──
    void draw_borders(litehtml::uint_ptr, const litehtml::borders& b, const litehtml::position& draw_pos, bool) override {
        if (litehtml::g_paint_trace) return;
        auto visible = [](const litehtml::border& s) { return float(s.width) > 0 && s.style != litehtml::border_style_none && s.style != litehtml::border_style_hidden && s.color.alpha != 0; };
        bool l = visible(b.left), t = visible(b.top), rr = visible(b.right), bt = visible(b.bottom);
        if (!l && !t && !rr && !bt) return;
        auto same = [](const litehtml::border& a, const litehtml::border& c) { return a.width == c.width && a.color == c.color && a.style == c.style; };
        auto broken = [](const litehtml::border& s) { return s.style == litehtml::border_style_dashed || s.style == litehtml::border_style_dotted; };
        Radii corners = radii_of(b.radius);
        bool square = corners.tlx == 0 && corners.tly == 0 && corners.trx == 0 && corners.try_ == 0 && corners.brx == 0 && corners.bry == 0 &&
                      corners.blx == 0 && corners.bly == 0;
#ifdef TW_LITEHTML_TWIDGET
        // One style all round, rounded dashed or dotted, or double: as LHContainer draws it.
        const bool all = l && t && rr && bt && same(b.left, b.top) && same(b.left, b.right) && same(b.left, b.bottom);
        if (all && ((!square && broken(b.left)) || (b.left.style == litehtml::border_style_double && float(b.left.width) >= 3))) {
            draw_uniform_styled(b.left, rect_of(draw_pos), corners);
            return;
        }
        if (square && ((l && broken(b.left)) || (t && broken(b.top)) || (rr && broken(b.right)) || (bt && broken(b.bottom)) ||
                       !(same(b.left, b.top) && same(b.left, b.right) && same(b.left, b.bottom)))) {
            draw_sides(b, rect_of(draw_pos), {l, t, rr, bt});
            return;
        }
#endif
        bool uniform = l && t && rr && bt && same(b.left, b.top) && same(b.left, b.right) && same(b.left, b.bottom);
        if (!square && !uniform) {
            draw_rounded_sides(b, rect_of(draw_pos), corners, {l, t, rr, bt});
            return;
        }
        if (uniform) {
            float w = float(b.left.width);
            Command c;
            c.kind = Cmd::StrokeRect;
            c.width = w;
            c.color = color_of(b.left.color);
            c.rect = Rect{float(draw_pos.x) + w / 2, float(draw_pos.y) + w / 2, float(draw_pos.width) - w, float(draw_pos.height) - w};
            Radii r = radii_of(b.radius);
            auto shrink = [&](float& v) { v = std::max(0.0f, v - w / 2); };
            shrink(r.tlx); shrink(r.tly); shrink(r.trx); shrink(r.try_); shrink(r.brx); shrink(r.bry); shrink(r.blx); shrink(r.bly);
            c.radii = r;
            list_.push_back(std::move(c));
            return;
        }
        Rect p = rect_of(draw_pos);
        auto strip = [&](const litehtml::border& s, Rect rect) {
            Command c;
            c.kind = Cmd::FillRect;
            c.color = color_of(s.color);
            c.rect = rect;
            list_.push_back(std::move(c));
        };
        if (t) strip(b.top, Rect{p.x, p.y, p.w, float(b.top.width)});
        if (bt) strip(b.bottom, Rect{p.x, p.y + p.h - float(b.bottom.width), p.w, float(b.bottom.width)});
        if (l) strip(b.left, Rect{p.x, p.y, float(b.left.width), p.h});
        if (rr) strip(b.right, Rect{p.x + p.w - float(b.right.width), p.y, float(b.right.width), p.h});
    }

    /// Appends `r` with corners `k` to `path` as one closed figure.
    static void rounded_rect(Path& path, const Rect& r, const Radii& k) {
        constexpr float K = 0.5523f;  // a quarter circle as one cubic
        const float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
        auto line = [&](float x, float y) {
            path.ops.push_back(Path::Line);
            path.points.insert(path.points.end(), {x, y});
        };
        auto curve = [&](float ax, float ay, float bx, float by, float x, float y) {
            path.ops.push_back(Path::Cubic);
            path.points.insert(path.points.end(), {ax, ay, bx, by, x, y});
        };
        path.ops.push_back(Path::Move);
        path.points.insert(path.points.end(), {x0 + k.tlx, y0});
        line(x1 - k.trx, y0);
        curve(x1 - k.trx + k.trx * K, y0, x1, y0 + k.try_ - k.try_ * K, x1, y0 + k.try_);
        line(x1, y1 - k.bry);
        curve(x1, y1 - k.bry + k.bry * K, x1 - k.brx + k.brx * K, y1, x1 - k.brx, y1);
        line(x0 + k.blx, y1);
        curve(x0 + k.blx - k.blx * K, y1, x0, y1 - k.bly + k.bly * K, x0, y1 - k.bly);
        line(x0, y0 + k.tly);
        curve(x0, y0 + k.tly - k.tly * K, x0 + k.tlx - k.tlx * K, y0, x0 + k.tlx, y0);
        path.ops.push_back(Path::Close);
    }

    /// How Chromium breaks a dashed or dotted border `width` thick along `length`: dashes three widths
    /// long with two-width gaps under 3 device px and two with one from there; dots square up to 3 device px
    /// and round past it. The gap stretches or shrinks so that a whole number of dashes fits: ending in a dash at
    /// both ends of an open side, or going round a closed one evenly. Solid when there is no room.
    struct BorderDash {
        bool solid = true;
        bool round = false;
        float dash = 0, gap = 0;
    };
    /// `dpr`: the 3 px thresholds are device pixels, not CSS px (OBSERVED, Chromium 156: a 2 px dotted border has square
    /// dots at DPR 1 and round ones at DPR 2, a 2 px dashed one 3:2 dashes at DPR 1 and 2:1 at DPR 2).
    static BorderDash border_dash(float length, float width, bool dotted, bool closed, float dpr) {
        BorderDash out;
        if (width <= 0 || length <= 0) return out;
        if (!dotted || width * dpr <= 3) {
            float dash = width, gap = width;
            if (!dotted) {
                dash *= width * dpr >= 3 ? 2 : 3;
                gap *= width * dpr >= 3 ? 1 : 2;
            }
            if (length <= dash * 2) return out;
            float pair = closed ? dash + gap : 2 * dash + gap;
            out.solid = false;
            if (length <= pair) {
                out.dash = dash * length / pair;
                out.gap = gap * length / pair;
            } else {
                out.dash = dash;
                out.gap = dotted ? gap : best_dash_gap(length, dash, gap, closed);
            }
            return out;
        }
        out.solid = false;
        out.round = true;
        if (length < 2 * width) {
            out.gap = 2 * width;
            return out;
        }
        // Round caps draw each zero-length dash as a dot a width across.
        out.gap = best_dash_gap(length, width, width, closed) + width - 0.01f;
        if (!std::isfinite(out.gap)) out.gap = length + 2 * width;
        return out;
    }
    static void apply_dash(Path& path, const BorderDash& d) {
        path.line_cap = d.round ? 1 : 0;
        if (!d.solid) path.dashes = {d.dash, d.gap};
    }
    /// A dashed or dotted rounded border: its centre line `mid` (corners `mrad`) stroked with the dashes
    /// fitted round it, under a clip to the ring between the border's edges so that dash ends follow the
    /// ring. Square dashes are stroked wider than the border (`reach`) for the clip to cut them.
    void dashed_ring(const Rect& outer_rect, const Radii& outer, const Rect& in, const Radii& inner, const Rect& mid, const Radii& mrad,
                     float width, float reach, bool dotted, const Color& color) {
        BorderDash d = border_dash(std::floor(rounded_length(mid, mrad)), width, dotted, true, float(r_.dpr_));
        auto ring = std::make_shared<Path>();
        rounded_rect(*ring, outer_rect, outer);
        rounded_rect(*ring, in, inner);
        ring->fill = true;
        ring->evenodd = true;
        Command clip;
        clip.kind = Cmd::ClipPush;
        clip.rect = outer_rect;
        clip.path = std::move(ring);
        clips_.push_back(clip);
        list_.push_back(std::move(clip));
        auto line = std::make_shared<Path>();
        rounded_rect(*line, mid, mrad);
        line->stroke = true;
        line->stroke_color = color;
        line->stroke_width = d.round ? width : reach * 2.2f;
        apply_dash(*line, d);
        Command c;
        c.kind = Cmd::Path;
        c.path = std::move(line);
        list_.push_back(std::move(c));
        clips_.pop_back();
        Command pop;
        pop.kind = Cmd::ClipPop;
        list_.push_back(std::move(pop));
    }

    /// Rounded borders whose sides differ: each side fills the ring between the outer and the inner
    /// rounded box, clipped to the part of the box it owns. That part runs from its two outer corners
    /// along the lines to their inner corners, where browsers split a corner between two colours, on to
    /// the box's midline or to where the two lines meet. A dashed or dotted side keeps its part of the
    /// centre line dashed all the way round.
    void draw_rounded_sides(const litehtml::borders& b, const Rect& p, const Radii& outer, std::array<bool, 4> visible) {
        const litehtml::border* sides[4] = {&b.left, &b.top, &b.right, &b.bottom};
        const float wl = float(b.left.width), wt = float(b.top.width), wr = float(b.right.width), wb = float(b.bottom.width);
        const Rect in{p.x + wl, p.y + wt, std::max(0.0f, p.w - wl - wr), std::max(0.0f, p.h - wt - wb)};
        const Radii inner{std::max(0.0f, outer.tlx - wl), std::max(0.0f, outer.tly - wt), std::max(0.0f, outer.trx - wr), std::max(0.0f, outer.try_ - wt),
                          std::max(0.0f, outer.brx - wr), std::max(0.0f, outer.bry - wb), std::max(0.0f, outer.blx - wl), std::max(0.0f, outer.bly - wb)};
        const float x0 = p.x, y0 = p.y, x1 = p.x + p.w, y1 = p.y + p.h, cx = p.x + p.w / 2, cy = p.y + p.h / 2;
        // Corners clockwise from the top left: the point, then the way to its inner corner.
        const float corner[4][4] = {{x0, y0, wl, wt}, {x1, y0, -wr, wt}, {x1, y1, -wr, -wb}, {x0, y1, wl, -wb}};
        for (int i = 0; i < 4; i++) {
            if (!visible[i]) continue;
            // Left runs from the bottom left corner to the top left one, top from there to the top right, and so on.
            const float* a = corner[(i + 3) % 4];
            const float* c = corner[i];
            const bool vertical = i == 0 || i == 2;
            // How far along a corner's line the midline is. The side's own width is the component that
            // moves towards the midline, and it is not 0 for a visible side.
            auto reach = [&](const float* k) { return vertical ? (cx - k[0]) / k[2] : (cy - k[1]) / k[3]; };
            const float ta = reach(a), tc = reach(c);
            std::vector<float> points{a[0], a[1], c[0], c[1]};
            // Where the two lines meet before the midline, as on a short side of a long box.
            const float rx = c[0] - a[0], ry = c[1] - a[1];
            const float det = c[2] * a[3] - a[2] * c[3];
            const float s = det != 0 ? (c[2] * ry - c[3] * rx) / det : -1;
            const float u = det != 0 ? (a[2] * ry - a[3] * rx) / det : -1;
            if (s > 0 && s < ta && u > 0 && u < tc) {
                points.insert(points.end(), {a[0] + s * a[2], a[1] + s * a[3]});
            } else {
                points.insert(points.end(), {c[0] + tc * c[2], c[1] + tc * c[3], a[0] + ta * a[2], a[1] + ta * a[3]});
            }
            auto share = std::make_shared<Path>();
            float minx = points[0], miny = points[1], maxx = points[0], maxy = points[1];
            for (size_t k = 0; k < points.size(); k += 2) {
                share->ops.push_back(k ? Path::Line : Path::Move);
                share->points.insert(share->points.end(), {points[k], points[k + 1]});
                minx = std::min(minx, points[k]); maxx = std::max(maxx, points[k]);
                miny = std::min(miny, points[k + 1]); maxy = std::max(maxy, points[k + 1]);
            }
            share->ops.push_back(Path::Close);

            Command clip;
            clip.kind = Cmd::ClipPush;
            clip.rect = Rect{minx, miny, maxx - minx, maxy - miny};
            clip.path = std::move(share);
            clips_.push_back(clip);
            list_.push_back(std::move(clip));
            if (sides[i]->style == litehtml::border_style_dashed || sides[i]->style == litehtml::border_style_dotted) {
                // The whole centre line, dashed round as one, of which the share keeps this side's part.
                const Rect mid{x0 + wl / 2, y0 + wt / 2, p.w - (wl + wr) / 2, p.h - (wt + wb) / 2};
                const Radii mrad{std::max(0.0f, outer.tlx - wl / 2), std::max(0.0f, outer.tly - wt / 2), std::max(0.0f, outer.trx - wr / 2),
                                 std::max(0.0f, outer.try_ - wt / 2), std::max(0.0f, outer.brx - wr / 2), std::max(0.0f, outer.bry - wb / 2),
                                 std::max(0.0f, outer.blx - wl / 2), std::max(0.0f, outer.bly - wb / 2)};
                const float w = float(sides[i]->width);
                const float reach = vertical ? std::max({w, wt, wb}) : std::max({w, wl, wr});
                dashed_ring(p, outer, in, inner, mid, mrad, w, reach, sides[i]->style == litehtml::border_style_dotted, color_of(sides[i]->color));
            } else {
                auto ring = std::make_shared<Path>();
                rounded_rect(*ring, p, outer);
                rounded_rect(*ring, in, inner);
                ring->fill = true;
                ring->evenodd = true;
                ring->fill_color = color_of(sides[i]->color);
                Command fill;
                fill.kind = Cmd::Path;
                fill.path = std::move(ring);
                list_.push_back(std::move(fill));
            }
            clips_.pop_back();
            Command pop;
            pop.kind = Cmd::ClipPop;
            list_.push_back(std::move(pop));
        }
    }

#ifdef TW_LITEHTML_TWIDGET
    /// The length of a rounded box's outline, its corners as quarter ellipses (Ramanujan).
    static float rounded_length(const Rect& r, const Radii& k) {
        if (r.w <= 0 || r.h <= 0) return 0;
        auto cx = [&](float v) { return std::min(std::max(v, 0.0f), r.w / 2); };
        auto cy = [&](float v) { return std::min(std::max(v, 0.0f), r.h / 2); };
        auto corner = [](float a, float b) {
            if (a <= 0 || b <= 0) return std::hypot(a, b);
            return 0.7853982f * (3 * (a + b) - std::sqrt((3 * a + b) * (a + 3 * b)));
        };
        const float xs[4] = {cx(k.tlx), cx(k.trx), cx(k.brx), cx(k.blx)}, ys[4] = {cy(k.tly), cy(k.try_), cy(k.bry), cy(k.bly)};
        float length = 2 * (r.w + r.h);
        for (int i = 0; i < 4; i++) length += corner(xs[i], ys[i]) - xs[i] - ys[i];
        return length;
    }

    /// The gap that fits whole dashes into `length` nearest to `gap` (LHContainer's BestDashGap).
    static float best_dash_gap(float length, float dash, float gap, bool closed) {
        const float fewer = std::floor((closed ? length : length + gap) / (dash + gap)), more = fewer + 1;
        const float fewer_gap = (length - fewer * dash) / (closed ? fewer : fewer - 1);
        const float more_gap = (length - more * dash) / (closed ? more : more - 1);
        return more_gap <= 0 || std::fabs(fewer_gap - gap) < std::fabs(more_gap - gap) ? fewer_gap : more_gap;
    }

    /// A uniform border's style that the solid paths cannot draw, as LHContainer draws it. Rounded
    /// dashes and dots are fitted round the closed centre line and stroked under a clip to the ring; a
    /// double border fills the outer and the inner third of the ring.
    void draw_uniform_styled(const litehtml::border& s, const Rect& p, const Radii& outer) {
        const float w = float(s.width);
        auto inset = [&](float d, Rect& r, Radii& k) {
            r = Rect{p.x + d, p.y + d, std::max(0.0f, p.w - 2 * d), std::max(0.0f, p.h - 2 * d)};
            k = Radii{std::max(0.0f, outer.tlx - d), std::max(0.0f, outer.tly - d), std::max(0.0f, outer.trx - d), std::max(0.0f, outer.try_ - d),
                      std::max(0.0f, outer.brx - d), std::max(0.0f, outer.bry - d), std::max(0.0f, outer.blx - d), std::max(0.0f, outer.bly - d)};
            return r.w > 0 && r.h > 0;
        };
        Rect r;
        Radii k;
        if (s.style == litehtml::border_style_double) {
            auto rings = std::make_shared<Path>();
            rounded_rect(*rings, p, outer);
            for (float d : {w / 3, 2 * w / 3, w})
                if (inset(d, r, k)) rounded_rect(*rings, r, k);
            rings->fill = true;
            rings->evenodd = true;
            rings->fill_color = color_of(s.color);
            Command c;
            c.kind = Cmd::Path;
            c.path = std::move(rings);
            list_.push_back(std::move(c));
            return;
        }
        Rect mid;
        Radii mrad;
        inset(w, r, k);
        inset(w / 2, mid, mrad);
        dashed_ring(p, outer, r, k, mid, mrad, w, w, s.style == litehtml::border_style_dotted, color_of(s.color));
    }

    /// Square borders side by side: each side is the trapezoid its corners mitre, filled when solid, and
    /// stroked along its middle under a clip to it when dashed or dotted, so that the dashes meet in the
    /// corners as browsers draw them.
    void draw_sides(const litehtml::borders& b, const Rect& p, std::array<bool, 4> visible) {
        const litehtml::border* sides[4] = {&b.left, &b.top, &b.right, &b.bottom};
        const float wl = float(b.left.width), wt = float(b.top.width), wr = float(b.right.width), wb = float(b.bottom.width);
        const float x0 = p.x, y0 = p.y, x1 = p.x + p.w, y1 = p.y + p.h;
        // Outer corners, then inner ones, clockwise from the top left.
        const float quads[4][8] = {
            {x0, y1, x0, y0, x0 + wl, y0 + wt, x0 + wl, y1 - wb},  // left
            {x0, y0, x1, y0, x1 - wr, y0 + wt, x0 + wl, y0 + wt},  // top
            {x1, y0, x1, y1, x1 - wr, y1 - wb, x1 - wr, y0 + wt},  // right
            {x1, y1, x0, y1, x0 + wl, y1 - wb, x1 - wr, y1 - wb},  // bottom
        };
        for (int i = 0; i < 4; i++) {
            if (!visible[i]) continue;
            const litehtml::border& s = *sides[i];
            auto quad = std::make_shared<Path>();
            for (int k = 0; k < 8; k += 2) {
                quad->ops.push_back(k ? Path::Line : Path::Move);
                quad->points.push_back(quads[i][k]);
                quad->points.push_back(quads[i][k + 1]);
            }
            quad->ops.push_back(Path::Close);
            const bool dashed = s.style == litehtml::border_style_dashed, dotted = s.style == litehtml::border_style_dotted;
            if (!dashed && !dotted) {
                quad->fill = true;
                quad->fill_color = color_of(s.color);
                Command c;
                c.kind = Cmd::Path;
                c.path = std::move(quad);
                list_.push_back(std::move(c));
                continue;
            }
            float w = float(s.width);
            bool horizontal = i == 1 || i == 3;
            // The middle of the side, corner to corner, where the dashes are fitted whole.
            float ax = horizontal ? x0 : (i == 0 ? x0 + w / 2 : x1 - w / 2), ay = horizontal ? (i == 1 ? y0 + w / 2 : y1 - w / 2) : y0;
            float bx = horizontal ? x1 : ax, by = horizontal ? ay : y1;
            float length = horizontal ? p.w : p.h;
            auto line = std::make_shared<Path>();
            line->stroke = true;
            line->stroke_color = color_of(s.color);
            line->stroke_width = w;
            BorderDash d = border_dash(std::round(length), w, dotted, false, float(r_.dpr_));
            apply_dash(*line, d);
            if (d.round) {
                // A round dot sits inside each end, its cap reaching the corner.
                ax += horizontal ? w / 2 : 0; ay += horizontal ? 0 : w / 2;
                bx -= horizontal ? w / 2 : 0; by -= horizontal ? 0 : w / 2;
            }
            line->ops = {Path::Move, Path::Line};
            line->points = {ax, ay, bx, by};
            Command clip;
            clip.kind = Cmd::ClipPush;
            clip.rect = Rect{std::min({quads[i][0], quads[i][2], quads[i][4], quads[i][6]}), std::min({quads[i][1], quads[i][3], quads[i][5], quads[i][7]}), 0, 0};
            clip.rect.w = std::max({quads[i][0], quads[i][2], quads[i][4], quads[i][6]}) - clip.rect.x;
            clip.rect.h = std::max({quads[i][1], quads[i][3], quads[i][5], quads[i][7]}) - clip.rect.y;
            clip.path = std::move(quad);
            clips_.push_back(clip);
            list_.push_back(std::move(clip));
            Command c;
            c.kind = Cmd::Path;
            c.path = std::move(line);
            list_.push_back(std::move(c));
            clips_.pop_back();
            Command pop;
            pop.kind = Cmd::ClipPop;
            list_.push_back(std::move(pop));
        }
    }
#endif

    // ── chrome the renderer has none of ──
    void set_caption(const char*) override {}
    void set_base_url(const char*) override {}
    void link(const std::shared_ptr<litehtml::document>&, const litehtml::element::ptr&) override {}
    void on_anchor_click(const char*, const litehtml::element::ptr&) override {}  // navigation is the sandbox's business (openLink)
    void on_mouse_event(const litehtml::element::ptr&, litehtml::mouse_event) override {}
    void set_cursor(const char* cursor) override { cursor_ = cursor ? cursor : ""; }
    /// The `cursor` of the element the mouse is over, as the document computes it; empty when none.
    const std::string& cursor() const { return cursor_; }
    void transform_text(std::string& text, litehtml::text_transform tt) override {
        // ASCII only; the platforms' Unicode case mapping would need a backend call.
        switch (tt) {
        case litehtml::text_transform_uppercase: for (auto& c : text) c = char(std::toupper((unsigned char)c)); break;
        case litehtml::text_transform_lowercase: for (auto& c : text) c = char(std::tolower((unsigned char)c)); break;
        case litehtml::text_transform_capitalize: {
            bool start = true;
            for (auto& c : text) {
                if (start) c = char(std::toupper((unsigned char)c));
                start = c == ' ';
            }
            break;
        }
        default: break;
        }
    }
    void import_css(std::string& text, const std::string&, std::string&) override { text.clear(); }  // @import is dropped by the validator anyway
    void set_clip(const litehtml::position& pos, const litehtml::border_radiuses& r) override {
        if (litehtml::g_paint_trace) return;
        Command c;
        c.kind = Cmd::ClipPush;
        c.rect = rect_of(pos);
        c.radii = radii_of(r);
        clips_.push_back(c);
        list_.push_back(std::move(c));
    }
    void del_clip() override {
        if (litehtml::g_paint_trace) return;
        if (!clips_.empty()) clips_.pop_back();
        Command c;
        c.kind = Cmd::ClipPop;
        list_.push_back(std::move(c));
    }
#ifdef TW_LITEHTML_TWIDGET
    void set_clip_path(const litehtml::vector_path& path) override {
        if (litehtml::g_paint_trace) return;
        auto p = std::make_shared<Path>();
        p->ops.reserve(path.ops.size());
        for (auto op : path.ops) p->ops.push_back(Path::Op(op));
        p->points = path.points;
        p->evenodd = path.evenodd;
        float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
        for (size_t i = 0; i + 1 < p->points.size(); i += 2) {
            x0 = std::min(x0, p->points[i]); x1 = std::max(x1, p->points[i]);
            y0 = std::min(y0, p->points[i + 1]); y1 = std::max(y1, p->points[i + 1]);
        }
        Command c;
        c.kind = Cmd::ClipPush;
        c.rect = x0 <= x1 && y0 <= y1 ? Rect{x0, y0, x1 - x0, y1 - y0} : Rect{};
        c.path = std::move(p);
        clips_.push_back(c);
        list_.push_back(std::move(c));
    }
    bool point_in_clip_path(const litehtml::vector_path& path, litehtml::pixel_t x, litehtml::pixel_t y) override {
        Geometry g;
        g.ops.reserve(path.ops.size());
        for (auto op : path.ops) g.ops.push_back(Path::Op(op));
        g.points = path.points;
        return geometry_contains(g, float(x), float(y), path.evenodd);
    }
    // A fixed element outside every group escapes its ancestors' overflow clips, as in WebKit: the clips
    // are popped around it and pushed again after.
    void suspend_clips() override {
        if (litehtml::g_paint_trace) return;
        Command pop;
        pop.kind = Cmd::ClipPop;
        for (size_t i = 0; i < clips_.size(); i++) list_.push_back(pop);
        suspended_.push_back(std::move(clips_));
        clips_.clear();
    }
    void resume_clips() override {
        if (litehtml::g_paint_trace) return;
        if (suspended_.empty()) return;
        Command pop;
        pop.kind = Cmd::ClipPop;
        for (size_t i = 0; i < clips_.size(); i++) list_.push_back(pop);
        clips_ = std::move(suspended_.back());
        suspended_.pop_back();
        for (const Command& c : clips_) list_.push_back(c);
    }
#endif
#ifdef TW_LITEHTML_TWIDGET
    /// The fork's filter functions for the display list; drop-shadow's currentColor is the element's colour.
    static std::shared_ptr<const Filters> filters_of(const litehtml::filter_paint& f) {
        if (!f.steps || f.steps->empty()) return nullptr;
        auto out = std::make_shared<Filters>();
        const auto* owner = static_cast<const litehtml::element*>(f.owner);
        for (const litehtml::filter_function& step : *f.steps) {
            Filter filter;
            filter.kind = Filter::Kind(int(step.kind));
            filter.amount = step.kind == litehtml::filter_function::blur || step.kind == litehtml::filter_function::drop_shadow ? step.radius.val() : step.amount;
            if (step.kind == litehtml::filter_function::drop_shadow) {
                filter.x = step.x.val();
                filter.y = step.y.val();
                filter.color = color_of(step.color.is_current_color && owner ? owner->css().get_color() : step.color);
            }
            out->push_back(filter);
        }
        return out;
    }
    void draw_filtered(litehtml::uint_ptr hdc, const litehtml::filter_paint& f, const std::function<void(litehtml::uint_ptr)>& paint) override {
        auto filters = litehtml::g_paint_trace || no_effects_ ? nullptr : filters_of(f);
        if (!filters) {
            paint(hdc);
            return;
        }
        Command push;
        push.kind = Cmd::FilterPush;
        push.rect = rect_of(f.bounds);
        push.filter = std::move(filters);
        list_.push_back(std::move(push));
        paint(hdc);
        Command pop;
        pop.kind = Cmd::FilterPop;
        list_.push_back(std::move(pop));
    }
    void draw_backdrop(litehtml::uint_ptr, const litehtml::filter_paint& f) override {
        if (litehtml::g_paint_trace || no_effects_) return;
        auto filters = filters_of(f);
        if (!filters) return;
        Command c;
        c.kind = Cmd::Backdrop;
        c.rect = rect_of(f.box);
        c.radii = radii_of(f.radius);
        c.filter = std::move(filters);
        list_.push_back(std::move(c));
    }
    void draw_box_shadow(litehtml::uint_ptr, const litehtml::position& box, const litehtml::border_radiuses& radius, const litehtml::shadow_paint& shadow) override {
        if (litehtml::g_paint_trace) return;
        if (shadow.color.alpha == 0 || (no_effects_ && float(shadow.blur) > 0)) return;
        Command c;
        c.kind = Cmd::Shadow;
        c.rect = rect_of(box);
        c.radii = radii_of(radius);
        c.color = color_of(shadow.color);
        c.ax = float(shadow.x);
        c.ay = float(shadow.y);
        c.bx = float(shadow.blur);
        c.by = float(shadow.spread);
        c.inset = shadow.inset;
        list_.push_back(std::move(c));
    }
    void begin_group(litehtml::uint_ptr, const litehtml::paint_group& group) override {
        if (litehtml::g_paint_trace) return;
        // Backface visibility needs the accumulated matrix: a flip undone by an outer flip is a front face.
        litehtml::transform_matrix total = groups_.empty() ? group.matrix : group.matrix.then(groups_.back());
        groups_.push_back(total);
        Command c;
        c.kind = Cmd::GroupPush;
        const auto& m = group.matrix;
        float values[6] = {m.a, m.b, m.c, m.d, m.tx, m.ty};
        std::copy(std::begin(values), std::end(values), c.matrix);
        c.opacity = group.opacity;
        c.hidden = group.backface_hidden && total.a * total.d - total.b * total.c < 0;
        if (no_effects_ && c.opacity < 1) {
            // A box that was invisible stays so, without a group to blend; a disabled button keeps its dimming.
            if (c.opacity <= 0) c.hidden = true;
            else if (!(group.owner && group.owner->tag() == litehtml::_button_ && group.owner->get_attr("disabled"))) c.opacity = 1;
        }
        c.rect = rect_of(group.bounds);
        list_.push_back(std::move(c));
    }
    void end_group(litehtml::uint_ptr) override {
        if (litehtml::g_paint_trace) return;
        if (!groups_.empty()) groups_.pop_back();
        Command c;
        c.kind = Cmd::GroupPop;
        list_.push_back(std::move(c));
    }
    /// An SVG paint under a key that stands for all it draws: recorded once, then copied in, shifted by
    /// how far the box moved, inside a keyed group a painter may raster once (Cmd::GroupPush).
    bool draw_cached(litehtml::uint_ptr hdc, const litehtml::position& box, uint64_t key, const std::function<void(litehtml::uint_ptr)>& paint) override {
        // The trace walks into the paint for its elements.
        if (litehtml::g_paint_trace) return false;
        Rect r = rect_of(box);
        if (r.empty()) return false;
        Command push;
        push.kind = Cmd::GroupPush;
        push.key = key;
        push.rect = r;
        Command pop;
        pop.kind = Cmd::GroupPop;
        auto it = cached_.find(key);
        if (it != cached_.end()) {
            CachedPaint& hit = it->second;
            // The key holds the fractional offset: a move is whole pixels.
            push.matrix[4] = r.x - hit.x;
            push.matrix[5] = r.y - hit.y;
            push.rect = Rect{hit.x, hit.y, r.w, r.h};
            hit.used = ++cached_clock_;
            list_.push_back(push);
            list_.insert(list_.end(), hit.commands.begin(), hit.commands.end());
            list_.push_back(pop);
            return true;
        }
        list_.push_back(push);
        size_t start = list_.size();
        paint(hdc);
        size_t count = list_.size() - start;
        if (count <= MAX_CACHED_COMMANDS / 4) {
            while (!cached_.empty() && (cached_.size() >= MAX_CACHED_PAINTS || cached_commands_ + count > MAX_CACHED_COMMANDS)) {
                auto oldest = cached_.begin();
                for (auto i = cached_.begin(); i != cached_.end(); ++i)
                    if (i->second.used < oldest->second.used) oldest = i;
                cached_commands_ -= oldest->second.commands.size();
                cached_.erase(oldest);
            }
            CachedPaint& entry = cached_[key];
            entry.x = r.x;
            entry.y = r.y;
            entry.used = ++cached_clock_;
            entry.commands.assign(list_.begin() + long(start), list_.end());
            cached_commands_ += count;
        }
        list_.push_back(pop);
        return true;
    }
    void draw_path(litehtml::uint_ptr, const litehtml::vector_path& path) override {
        if (litehtml::g_paint_trace) return;
        if (!path.fill && !path.stroke) return;
        auto p = std::make_shared<Path>();
        p->ops.reserve(path.ops.size());
        for (auto op : path.ops) p->ops.push_back(Path::Op(op));
        p->points = path.points;
        p->fill = path.fill;
        p->evenodd = path.evenodd;
        p->fill_color = color_of(path.fill_color);
        const auto& g = path.fill_gradient;
        p->gradient = g.type;
        p->x1 = g.x1; p->y1 = g.y1; p->x2 = g.x2; p->y2 = g.y2; p->r = g.r; p->fx = g.fx; p->fy = g.fy;
        for (const auto& [offset, color] : g.stops) p->stops.push_back(Stop{offset, color_of(color)});
        p->stroke = path.stroke;
        p->stroke_color = color_of(path.stroke_color);
        p->stroke_width = path.stroke_width;
        p->line_cap = path.line_cap;
        p->line_join = path.line_join;
        p->miter_limit = path.miter_limit;
        p->dashes = path.dashes;
        p->dash_offset = path.dash_offset;
        const auto& sg = path.stroke_gradient;
        p->stroke_gradient = sg.type;
        const float sa[7] = {sg.x1, sg.y1, sg.x2, sg.y2, sg.r, sg.fx, sg.fy};
        std::copy(std::begin(sa), std::end(sa), p->sg);
        for (const auto& [offset, color] : sg.stops) p->stroke_stops.push_back(Stop{offset, color_of(color)});
        Command c;
        c.kind = Cmd::Path;
        c.path = std::move(p);
        list_.push_back(std::move(c));
    }
#endif
    void get_viewport(litehtml::position& v) const override {
        v.x = 0; v.y = 0;
        v.width = litehtml::pixel_t(float(r_.width_));
        v.height = litehtml::pixel_t(float(vp_height_));
    }
    litehtml::element::ptr create_element(const char*, const litehtml::string_map&, const std::shared_ptr<litehtml::document>&) override { return nullptr; }
    void get_media_features(litehtml::media_features& m) const override {
        m.type = litehtml::media_type_screen;
        m.width = litehtml::pixel_t(float(r_.width_));
        m.height = litehtml::pixel_t(float(vp_height_));
        m.device_width = m.width;
        m.device_height = m.height;
        m.color = 8;
        m.color_index = 256;
        m.monochrome = 0;
        m.resolution = litehtml::pixel_t(float(96.0 * r_.dpr_));
    }
    void get_language(std::string& language, std::string& culture) const override {
        language = "en";
        culture.clear();
    }

private:
    /// A backend font for one description, shared by every document and font handle asking for it,
    /// with the widths measured in it.
    struct Face {
        int handle = 0;
        FontMetrics metrics;
        int uses = 0;  // litehtml font handles on it
        uint64_t used = 0;
        struct Hash {
            using is_transparent = void;
            size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
        };
        std::unordered_map<std::string, float, Hash, std::equal_to<>> widths;
        size_t* pool = nullptr;  // the key bytes every face holds
        size_t bytes = 0;        // this face's share of them
        Face() = default;
        Face(const Face&) = delete;
        Face& operator=(const Face&) = delete;
        ~Face() { forget(); }
        void forget() {
            widths.clear();
            if (pool) *pool -= bytes;
            bytes = 0;
        }
        float width(Backend& backend, std::string_view text) {
            if (auto c = widths.find(text); c != widths.end()) return c->second;
            float w = backend.text_width(handle, std::string(text));
            // Words repeat across documents, but a widget that types a counter would grow this forever.
            if (text.size() > MAX_WIDTH_KEY) return w;
            if (widths.size() >= MAX_WIDTH_ENTRIES || (pool && *pool + text.size() > MAX_WIDTH_BYTES)) forget();
            if (pool && *pool + text.size() > MAX_WIDTH_BYTES) return w;  // other faces hold the budget
            widths.emplace(std::string(text), w);
            bytes += text.size();
            if (pool) *pool += text.size();
            return w;
        }
    };
    using FaceKey = std::tuple<std::string, float, int, bool, std::string>;
    Face* face_for(const std::string& family, float size, int weight, bool italic, const std::string& features = {}) {
        FaceKey key{family, size, weight, italic, features};
        auto it = faces_.find(key);
        if (it == faces_.end()) {
            // Past the bound, faces no document holds go, least recently used first.
            if (faces_.size() >= MAX_FACES) {
                std::vector<std::pair<uint64_t, FaceKey>> idle;
                for (const auto& [k, f] : faces_)
                    if (f->uses == 0) idle.emplace_back(f->used, k);
                std::sort(idle.begin(), idle.end());
                for (size_t i = 0; i < idle.size() && faces_.size() >= MAX_FACES / 2; i++) {
                    backend_.delete_font(faces_[idle[i].second]->handle);
                    faces_.erase(idle[i].second);
                }
#ifdef TW_LITEHTML_TWIDGET
                // A cached paint may draw text in a face gone now.
                cached_.clear();
                cached_commands_ = 0;
#endif
            }
            auto face = std::make_unique<Face>();
            face->pool = &width_bytes_;
            face->handle = backend_.create_font(family, size, weight, italic, features, face->metrics);
            if (!face->handle) return nullptr;
            it = faces_.emplace(std::move(key), std::move(face)).first;
        }
        it->second->used = ++face_clock_;
        return it->second.get();
    }
    struct Font {
        Face* face = nullptr;
        int handle = 0;
        FontMetrics metrics;
        float size = 16;
        int decoration = 0;
        litehtml::web_color decoration_color;
        float letter_spacing = 0, word_spacing = 0;
        float width(Backend& backend, std::string_view text) const {
            float w = face->width(backend, text);
            if (letter_spacing != 0) w += letter_spacing * float(spacing_units(text).size());
            if (word_spacing != 0) w += word_spacing * float(spacing_spaces(text));
            return w;
        }
    };
    struct Image {
        int id = 0;
        int canvas = 0;
        bool placeholder = false;
        float w = 0, h = 0;
    };

#ifdef TW_LITEHTML_TWIDGET
    static void positioned_z(const std::shared_ptr<litehtml::render_item>& item, std::set<int>& z) {
        for (const auto& child : item->children()) {
            if (child->src_el()->is_positioned()) z.insert(child->src_el()->css().get_z_index());
            else if (!child->src_el()->css().is_paint_group()) positioned_z(child, z);
        }
    }
    /// An element's children in stacking order, as if it formed a stacking context: what litehtml's
    /// own group paint does after the element's background, without the group.
    static void draw_stacking_context(const std::shared_ptr<litehtml::render_item>& ri, litehtml::pixel_t x, litehtml::pixel_t y) {
        std::set<int> z;
        positioned_z(ri, z);
        for (int index : z)
            if (index < 0) ri->draw_children(0, x, y, nullptr, litehtml::draw_positioned, index);
        ri->draw_children(0, x, y, nullptr, litehtml::draw_block, 0);
        ri->draw_children(0, x, y, nullptr, litehtml::draw_floats, 0);
        ri->draw_children(0, x, y, nullptr, litehtml::draw_inlines, 0);
        for (int index : z)
            if (index >= 0) ri->draw_children(0, x, y, nullptr, litehtml::draw_positioned, index);
    }
#endif

    static void clip_intersect(Rect& a, const Rect& b) {
        float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
        float x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
        a = Rect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }

    static std::string marker_text(litehtml::list_style_type t, int index) {
        // Out of a style's range (css-counter-styles: alphabetic from 1, roman 1 to 3999) the marker is decimal, as
        // browsers draw it; roman past it was one more 'M' per thousand, two million for <ol start=2000000000>.
        auto alpha = [](int n, char base) {
            if (n < 1) return std::to_string(n);
            std::string s;
            while (n > 0) { n--; s.insert(s.begin(), char(base + n % 26)); n /= 26; }
            return s;
        };
        auto roman = [](int n, bool upper) {
            if (n < 1 || n > 3999) return std::to_string(n);
            static const char* U[] = {"M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"};
            static const char* L[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
            static const int V[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
            std::string s;
            for (int i = 0; i < 13; i++) while (n >= V[i]) { s += upper ? U[i] : L[i]; n -= V[i]; }
            return s;
        };
        switch (t) {
        case litehtml::list_style_type_decimal_leading_zero: return (index < 10 ? "0" : "") + std::to_string(index) + ".";
        case litehtml::list_style_type_lower_alpha:
        case litehtml::list_style_type_lower_latin: return alpha(index, 'a') + ".";
        case litehtml::list_style_type_upper_alpha:
        case litehtml::list_style_type_upper_latin: return alpha(index, 'A') + ".";
        case litehtml::list_style_type_lower_roman: return roman(index, false) + ".";
        case litehtml::list_style_type_upper_roman: return roman(index, true) + ".";
        default: return std::to_string(index) + ".";
        }
    }

    Renderer& r_;
    Backend& backend_;
    litehtml::document::ptr doc_;
#ifdef TW_LITEHTML_TWIDGET
    std::vector<litehtml::transform_matrix> groups_;  // accumulated, during draw()
#endif
    int vp_height_ = int(MIN_HEIGHT);
    float rendered_width_ = 0;
    bool viewport_units_ = false;  // the current document has lengths folded from the viewport
    std::string master_css_;
#ifdef TW_LITEHTML_TWIDGET
    struct Scan {
        std::vector<std::string> names, gates;
        std::vector<int64_t> always;
        uint64_t epoch;
        uint64_t generation;
        std::unordered_map<int64_t, Declared> result;
    };
    mutable uint64_t scan_generation_ = 0;
    mutable std::vector<Scan> scans_;  // declarations(), latest first; a new document starts its epochs again
#endif
    uint64_t layouts_ = 0;
    bool patched_ = false;
    std::vector<const litehtml::render_item*> trace_;
    struct Cover {
        Rect box;
        bool invisible;
    };
    std::vector<Cover> cover_;  // parallel to the trace scan() was last given
    std::unordered_map<const litehtml::render_item*, size_t> cover_index_;
    const void* cover_trace_ = nullptr;
    std::tuple<const litehtml::document*, uint32_t, uint64_t> cover_key_{};
    std::tuple<const litehtml::document*, uint32_t, uint64_t> trace_key_{};
    std::unordered_map<int64_t, litehtml::element::ptr> elements_;
    std::string cursor_;
    size_t width_bytes_ = 0;  // before faces_, whose destruction gives its bytes back
    std::map<FaceKey, std::unique_ptr<Face>> faces_;
    uint64_t face_clock_ = 0;
    std::unordered_map<litehtml::uint_ptr, Font> fonts_;
    litehtml::uint_ptr next_font_ = 0;
    std::unordered_map<std::string, Image> images_;
    int next_image_ = 0;
    DisplayList list_;
    std::vector<Command> clips_;
    std::vector<std::vector<Command>> suspended_;
#ifdef TW_LITEHTML_TWIDGET
    struct CachedPaint {
        float x = 0, y = 0;  // where the box was when it was recorded
        uint64_t used = 0;
        std::vector<Command> commands;
    };
    std::unordered_map<uint64_t, CachedPaint> cached_;
    size_t cached_commands_ = 0;
    uint64_t cached_clock_ = 0;
#endif
    bool no_effects_ = false;

public:
    /// A widget stopped past the paint limit (paint_policy.h) is drawn without blurred shadows, filters,
    /// backdrop-filter or transparency, so its frozen frame costs nothing to scroll.
    void set_no_effects() {
        if (no_effects_) return;
        no_effects_ = true;
#ifdef TW_LITEHTML_TWIDGET
        cached_.clear();
        cached_commands_ = 0;
        if (doc_) doc_->invalidate_paint();
#endif
    }
};

// ───────────────────────── Renderer ─────────────────────────

Renderer::Renderer(double width, double dpr, bool dark, Backend& backend)
    : container_(std::make_unique<Container>(*this, backend)), backend_(backend), width_(width), dpr_(dpr), dark_(dark), vp_height_(int(MIN_HEIGHT)) {
    auto make = [](const char* tag) {
        auto n = std::make_unique<Node>();
        n->tag = tag;
        return n;
    };
    html_node_ = make("html");
    head_node_ = make("head");
    body_node_ = make("body");
    head_node_->parent = html_node_.get();
    body_node_->parent = html_node_.get();
    html_node_->children = {head_node_.get(), body_node_.get()};
    safe_area_vars_ = css::content_safe_area_variables(Json());
    // Keyframes and declared values with var() mix as their substituted values (as a browser computes them first).
    animator_.resolve_vars = [this](int64_t id, const std::string& value) { return container_->resolve_vars(id, value); };
    apply_theme(Json::object().set("colorScheme", dark ? "dark" : "light"));
    rebuild();
}

bool Renderer::apply_theme(const Json& theme) {
    auto vars = css::theme_variables(theme);
    if (vars == host_theme_vars_) return false;
    host_theme_vars_ = std::move(vars);
    apply_root_variables();
    return true;
}

bool Renderer::apply_safe_area(const Json& inset) {
    auto vars = css::content_safe_area_variables(inset);
    if (vars == safe_area_vars_) return false;
    safe_area_vars_ = std::move(vars);
    apply_root_variables();
    return true;
}

void Renderer::apply_root_variables() {
    theme_vars_ = host_theme_vars_;
    theme_vars_.insert(theme_vars_.end(), safe_area_vars_.begin(), safe_area_vars_.end());
    // The layer with the theme's values in place of its var(), since the fork applies a var() in a shorthand
    // after the cascade (`margin: var(…)` from the master CSS would beat the widget's own `margin: 0`), and
    // without :where() (css.h, unwrap_where).
    container_->set_master(std::string(litehtml::master_css) + UA_EXTRA + css::unwrap_where(css::substitute_variables(TW_UA_STYLESHEET, theme_vars_)));
}

const std::string& Renderer::master_css() const { return container_->master(); }

std::string Renderer::theme_variables_json() const {
    Json out = Json::object();
    for (const auto& [name, value] : theme_vars_) out.set(name, value);
    return out.dump();
}

std::string Renderer::cursor() const {
    const std::string& c = container_->cursor();
    return c.empty() || frozen_ ? "auto" : css::trim(lower(c));
}

Renderer::~Renderer() {
    for (const auto& [id, canvas] : canvases_) backend_.canvas_release(canvas->id());
    for (const auto& [css, font] : canvas_fonts_) backend_.delete_font(font.first);
}

double Renderer::now_s() { return now_ms() / 1000; }

void Renderer::set_test_clock(double seconds) { g_test_clock.store(seconds, std::memory_order_relaxed); }

uint32_t Renderer::px_width() const { return uint32_t(std::ceil(width_ * dpr_)); }
uint64_t Renderer::layouts() const { return container_->layouts(); }
uint32_t Renderer::px_height() const { return uint32_t(std::ceil(vp_height_ * dpr_)); }

std::vector<Json> Renderer::receive(const Json& msg) {
    std::vector<Json> out;
    const std::string& type = msg["type"].asString();
    if (type == "ops") {
        // Past the paint limit the host stops the widget: what its task still sent is not drawn.
        if (paint_stopped_) return out;
        const auto& ops = msg["ops"].arr();
        std::vector<int64_t> scripted;
        bool fast = scripted_only(ops, scripted);
        // Ticking already (something animates), or held back by the throttle, a batch waits for the
        // frame: a script sending several a frame builds once. Input wants its answer now. A part of a long
        // flush always waits: the rest of the task follows at once, and its last part builds them all.
        bool partial = ops.size() >= PARTIAL_BATCH_OPS;
        bool coalesce = partial || ((frame_due() || throttled()) && now_s() - last_input_ > INPUT_IMMEDIATE_S);
        document_changed_ = false;
        apply_ops(ops, out);
        load_images(out);
        if (fast && apply_scripted(scripted)) {
            document_changed_ = false;
            scripted_pending_ = true;
            stats_.scripted++;
            // The document takes these at its next build; their opacity counts now.
            recount_boxes(scripted);
        } else {
            // Moved past what its layer allows, a scripted layer is planned again where it is now.
            if (fast) composition_key_ = 0;
            else note_scripted(ops);
            note_svg(ops);
            // A batch of canvas drawing, scrolling or focus leaves the document as it was: its canvases
            // repaint (run_canvas marks the paint dirty) without a build.
            if (document_changed_ && coalesce) {
                stale_ = true;
            } else if (document_changed_ || (stale_ && !partial)) {
                rebuild(true);
                report_size(out);
            }
        }
        // A batch that built nothing can still have asked for new layers (a canvas drawn once more).
        if (composition_key_ == 0 && compositing_ && !stale_) update_composition(now_s());
    } else if (type == "query") {
        // A query reads the document, which must have what only the layers show.
        if (scripted_pending_) stale_ = true;
        settle(out);
        Json res = Json::object();
        binary_.clear();
        res.set("type", "result").set("qid", msg["qid"]);
        res.set("result", msg.find("query") ? answer(msg["query"]) : Json::object().set("error", "no query"));
        if (!binary_.empty()) res.set("binaryBase64", std::move(binary_));
        binary_.clear();
        out.push_back(std::move(res));
    } else if (type == "anim") {
        settle(out);
        if (!still()) {
            // `now` is the sandbox clock when the batch was sent: it maps the records' start times onto ours.
            double now = now_ms();
            animator_.apply_web(msg, now - msg["now"].asNumber(now));
            animate();
            report_size(out);
        }
    } else if (type == "listen") {
        interest_.clear();
        for (const Json& t : msg["types"].arr()) if (t.isString()) interest_.insert(t.asString());
    } else if (type == "preventDefault") {
        prevent_.clear();
        for (const Json& k : msg["keys"].arr()) if (k.isString()) prevent_.insert(k.asString());
    } else if (type == "theme") {
        theme_received_ = true;
        if (apply_theme(msg["theme"])) {
            rebuild();
            report_size(out);
        }
    } else if (type == "safeArea") {
        // The client's time over the widget (docs/sdk-v1.md §2.17): the UA layer pads the body by its band.
        if (apply_safe_area(msg["inset"])) {
            rebuild();
            report_size(out);
        }
    } else if (type == "frozen") {
        freeze(msg["reason"].asString(), msg["fatal"].asBool());
        rebuild();
    } else if (type == "policy") {
        // Heavy boxes are counted unless the host says compat (also before this message, and for anything else).
        bool on = msg["policy"].asString() != "compat";
        paint_reports_ = msg["paintReports"].asBool();
        paint_reported_ = {-1, -1};
        if (on && !paint_on_) {
            paint_scan_ = true;
            motion_stale_ = true;
        }
        paint_on_ = on;
        count_paint();
    } else if (type == "arbitrate") {
        bubble_ = msg["bubble"].asBool();
        bubble_touch_ = bubble_ && msg["touch"].asBool();
        rtl_ = msg["rtl"].asBool();
    }
    flush_pending(out);
    return out;
}

void Renderer::flush_pending(std::vector<Json>& out) {
    report_resizes(out);
    for (auto& ev : pending_events_) out.push_back(std::move(ev));
    pending_events_.clear();
    for (auto& v : pending_violations_) out.push_back(Json::object().set("type", "violation").set("detail", v));
    pending_violations_.clear();
}

void Renderer::toggle_details(int64_t clicked, std::vector<Json>& out) {
    const Node* summary = node(clicked);
    for (; summary && summary->tag != "summary"; summary = summary->parent)
        if (prevent_.count(std::to_string(summary->id) + ":click")) return;
    if (!summary || prevent_.count(std::to_string(summary->id) + ":click")) return;
    Node* details = summary->parent;
    if (!details || details->tag != "details") return;
    // Only its first summary opens it.
    for (const Node* c : details->children) {
        if (c->kind == Node::Element && c->tag == "summary") {
            if (c != summary) return;
            break;
        }
    }
    auto open = std::find_if(details->attrs.begin(), details->attrs.end(), [](const auto& a) { return a.first == "open"; });
    bool opening = open == details->attrs.end();
    if (opening) details->attrs.emplace_back("open", "");
    else details->attrs.erase(open);
    if (attached(details)) patch_log_.valid = false;
    stale_ = true;
    // The sandbox mirrors `open` from the event (widget-sandbox forms.ts).
    Json ev = event_json("toggle", details->id, box_of(details->id), Input{});
    ev.set("open", opening);
    dispatch(std::move(ev), out);
}

void Renderer::report_resizes(std::vector<Json>& out) {
    // A layout still to come reports on the frame that builds it.
    if (resize_watch_.empty() || frozen_ || stale_) return;
    Json entries = Json::array();
    for (auto& [id, last] : resize_watch_) {
        auto el = container_->element(id);
        litehtml::position content = el ? el->get_placement() : litehtml::position();
        std::pair<float, float> size{float(content.width), float(content.height)};
        if (size == last) continue;
        last = size;
        entries.push(Json::object().set("id", double(id)).set("width", size.first).set("height", size.second));
    }
    if (!entries.arr().empty()) out.push_back(Json::object().set("type", "resize").set("entries", std::move(entries)));
}

void Renderer::set_width(double width, std::vector<Json>& out) {
    if (std::fabs(width - width_) < 0.01) return;
    width_ = width;
    rebuild();
    report_size(out);
}

void Renderer::set_max_height(double height, std::vector<Json>& out) {
    height = std::clamp(height, MIN_HEIGHT, HEIGHT_CEILING);
    if (height == max_height_) return;
    max_height_ = height;
    rebuild();
    report_size(out);
}

void Renderer::set_theme(bool dark, std::vector<Json>& out) {
    if (dark == dark_) return;
    dark_ = dark;
    if (!theme_received_) apply_theme(Json::object().set("colorScheme", dark ? "dark" : "light"));
    rebuild();
    report_size(out);
}

// ───────────────────────── ops ─────────────────────────

Renderer::Node* Renderer::node(int64_t id) const {
    auto it = nodes_.find(id);
    if (it != nodes_.end()) return it->second.get();
    auto a = aliases_.find(id);
    return a == aliases_.end() ? nullptr : a->second;
}

void Renderer::apply_ops(const std::vector<Json>& ops, std::vector<Json>& out) {
    // The sandbox sends a canvas's commands as one op per canvas, the first canvas used first.
    std::unordered_map<int64_t, size_t> drawn;  // canvas node → its canvas op
    for (size_t i = 0; i < ops.size(); i++)
        if (ops[i]["op"].asString() == "canvas") drawn.emplace(int64_t(ops[i]["id"].asNumber()), i);
    if (drawn.size() < 2) {
        for (const Json& op : ops) apply_op(op, out);
        return;
    }
    std::vector<char> done(ops.size(), 0);
    std::function<void(size_t)> run = [&](size_t i) {
        if (done[i]) return;
        done[i] = 1;
        if (ops[i]["op"].asString() == "canvas") {
            for (const Json& cmd : ops[i]["cmds"].arr()) {
                if (cmd[size_t(1)].asString() != "drawImage") continue;
                const Json* source = cmd[size_t(2)].find("n");
                if (!source) continue;
                int64_t ref = int64_t(source->asNumber());
                auto it = drawn.find(ref);
                if (it == drawn.end() || it->second <= i || done[it->second]) continue;
                // The source's node and its size (a canvas made by script arrives just before its drawing);
                // where it is inserted stays in its place.
                for (size_t k = i + 1; k < it->second; k++) {
                    const std::string& kind = ops[k]["op"].asString();
                    if (!done[k] && int64_t(ops[k]["id"].asNumber()) == ref && (kind == "el" || kind == "attr")) run(k);
                }
                run(it->second);
            }
        }
        apply_op(ops[i], out);
    };
    for (size_t i = 0; i < ops.size(); i++) run(i);
}

void Renderer::apply_op(const Json& op, std::vector<Json>& out) {
    const std::string& kind = op["op"].asString();
    int64_t id = int64_t(op["id"].asNumber());
    auto str = [&](const char* k) -> const std::string& { return op[k].asString(); };
    // What changes the serialised document; `call` marks it itself when it toggles an attribute.
    if (kind != "canvas" && kind != "call" && !(kind == "prop" && (str("name") == "scrollTop" || str("name") == "scrollLeft"))) document_changed_ = true;
    if (kind == "el") {
        const std::string& adopt_name = str("adopt");
        if (!adopt_name.empty()) {
            Node* n = adopt_name == "html" ? html_node_.get() : adopt_name == "head" ? head_node_.get() : adopt_name == "body" ? body_node_.get() : nullptr;
            if (!n) return;
            patch_log_.valid = false;
            adopt(id, n, op["attrs"]);
            return;
        }
        if (nodes_.count(id)) return;  // the validator never reuses a live id; keep the first
        auto n = std::make_unique<Node>();
        n->kind = Node::Element;
        if (!css::is_tag_name(str("tag"))) {
            out.push_back(Json::object().set("type", "violation").set("detail", "element name"));
            return;
        }
        n->id = id;
        n->tag = lower(str("tag"));
        n->svg = op["ns"].asString() == "svg";
        for (const Json& pair : op["attrs"].arr()) {
            if (!pair[0].isString() || !pair[1].isString()) continue;
            if (starts_with_on(pair[0].asString())) continue;
            if (pair[0].asString() == "style") set_inline_style(n->styles, pair[1].asString());
            else n->attrs.emplace_back(pair[0].asString(), pair[1].asString());
        }
        if (n->tag == "img" && !n->svg) pending_images_.push_back(id);
        nodes_[id] = std::move(n);
        return;
    }
    if (kind == "text" || kind == "comment") {
        if (nodes_.count(id)) return;
        auto n = std::make_unique<Node>();
        n->kind = kind == "text" ? Node::Text : Node::Comment;
        n->id = id;
        n->data = str("data");
        nodes_[id] = std::move(n);
        return;
    }
    Node* n = node(id);
    if (!n) return;
    bool is_root = n == html_node_.get() || n == head_node_.get() || n == body_node_.get();
    if (kind == "attr") {
        const std::string& name = str("name");
        if (starts_with_on(name) || !css::is_attribute_name(name)) {
            out.push_back(Json::object().set("type", "violation").set("detail", "attribute " + name.substr(0, 64)));
            return;
        }
        note_attribute(n, lower(name));
        if (name == "style") {
            set_inline_style(n->styles, str("value"));
            return;
        }
        bool found = false;
        for (auto& a : n->attrs) if (a.first == name) { a.second = str("value"); found = true; }
        if (!found) n->attrs.emplace_back(name, str("value"));
        if (n->tag == "img" && !n->svg && name == "src") pending_images_.push_back(n->id);
        // Setting a canvas's width or height clears its bitmap, as in a browser.
        if (n->tag == "canvas" && (name == "width" || name == "height")) {
            if (auto it = canvases_.find(n->id); it != canvases_.end()) {
                int w = it->second->width(), h = it->second->height();
                (name == "width" ? w : h) = std::max(0, std::atoi(str("value").c_str()));
                std::vector<CanvasOp> ops;
                it->second->resize(w, h, ops);
                backend_.canvas_run(it->second->id(), it->second->width(), it->second->height(), ops);
            }
        }
    } else if (kind == "rattr") {
        const std::string& name = str("name");
        // hidden: a boolean attribute patch; style and class: back to the empty value their hole keeps (serialize_node).
        if (name == "hidden" || ((name == "style" || (name == "class" && !n->svg)) && attribute_holes(n))) note_attribute(n, lower(name));
        else {
            if (n->kind == Node::Element && n->id) restyled_[n->id] = true;
            if (attached(n)) patch_log_.valid = false;
        }
        if (name == "style") n->styles.clear();
        n->attrs.erase(std::remove_if(n->attrs.begin(), n->attrs.end(), [&](auto& a) { return a.first == name; }), n->attrs.end());
    } else if (kind == "data") {
        if (n->kind == Node::Element) return;
        note_children(n->parent, n);
        n->data = str("data");
    } else if (kind == "ins") {
        if (is_root) return;
        int64_t parent = int64_t(op["parent"].asNumber());
        note_children(n->parent, n);
        detach(n);
        if (parent == 0) return;
        Node* p = node(parent);
        if (!p || p->kind != Node::Element) return;
        note_children(p, n);
        // no cycles: the validator forbids them, but a hostile batch must not hang the renderer
        for (Node* a = p; a; a = a->parent) if (a == n) return;
        n->parent = p;
        const Json* before = op.find("before");
        if (before && before->isNumber()) {
            Node* b = node(int64_t(before->asNumber()));
            if (b && b->parent == p) {
                auto it = std::find(p->children.begin(), p->children.end(), b);
                p->children.insert(it, n);
                return;
            }
        }
        p->children.push_back(n);
    } else if (kind == "rm") {
        if (is_root) return;
        note_children(n->parent, n);
        detach(n);
        forget(n);
    } else if (kind == "style") {
        std::string name = css::starts_with(str("name"), "--") ? str("name") : lower(str("name"));
        if (!css::is_property_name(name)) {
            out.push_back(Json::object().set("type", "violation").set("detail", "property name"));
            return;
        }
        // The host's theme variables on the root are not the widget's to change (it can shadow them on its own elements).
        if (n == html_node_.get() && css::is_theme_variable(name)) return;
        note_attribute(n, "style");
        std::string value = str("value");
        if (op["important"].asBool() && !value.empty()) value += " !important";
        auto it = std::find_if(n->styles.begin(), n->styles.end(), [&](auto& s) { return s.first == name; });
        if (value.empty()) {
            if (it != n->styles.end()) n->styles.erase(it);
        } else if (it != n->styles.end()) it->second = value;
        else n->styles.emplace_back(name, value);
    } else if (kind == "css") {
        if (attached(n)) patch_log_.valid = false;
        if (n->tag == "style") n->data = str("text");
    } else if (kind == "prop") {
        const std::string& name = str("name");
        const Json& value = op["value"];
        auto set_attr = [&](const std::string& a, const std::string& v) {
            for (auto& p : n->attrs) if (p.first == a) { p.second = v; return; }
            n->attrs.emplace_back(a, v);
        };
        auto remove_attr = [&](const std::string& a) {
            n->attrs.erase(std::remove_if(n->attrs.begin(), n->attrs.end(), [&](auto& p) { return p.first == a; }), n->attrs.end());
        };
        if (name == "value") {
            std::string v = value.isString() ? value.asString() : value.isNull() ? std::string() : value.dump();
            if (n->tag == "textarea") {
                // A textarea's value is its text, a hole when it is the only content.
                if (!n->children.empty()) patch_log_.valid = false;
                else if (attached(n)) patch_log_.texts.insert(n);
                n->data = v;
                n->children.clear();
            } else {
                note_attribute(n, "value");
                set_attr("value", v);
            }
            if (editing_.id == n->id && editing_.text != v) {
                editing_.text = v;
                editing_.selection_start = editing_.selection_end = -1;
                editing_.generation = ++editor_generation_;
            }
        } else if (name == "checked" || name == "selected" || name == "indeterminate") {
            if (attached(n)) patch_log_.valid = false;
            if (value.asBool()) set_attr(name, ""); else remove_attr(name);
        } else if (name == "selectedIndex") {
            if (attached(n)) patch_log_.valid = false;
            int idx = value.asInt(-1), i = 0;
            for (Node* c : n->children) {
                if (c->tag != "option") continue;
                c->attrs.erase(std::remove_if(c->attrs.begin(), c->attrs.end(), [](auto& p) { return p.first == "selected"; }), c->attrs.end());
                if (i++ == idx) c->attrs.emplace_back("selected", "");
            }
        }
        // scrollTop/scrollLeft: litehtml has no scrolling boxes to move
    } else if (kind == "call") {
        const std::string& method = str("method");
        if (method == "showModal" || method == "show" || method == "showPopover") {
            for (auto& p : n->attrs) if (p.first == "open") return;
            n->attrs.emplace_back("open", "");
            document_changed_ = true;
            if (attached(n)) patch_log_.valid = false;
        } else if (method == "close" || method == "hidePopover") {
            n->attrs.erase(std::remove_if(n->attrs.begin(), n->attrs.end(), [](auto& p) { return p.first == "open"; }), n->attrs.end());
            document_changed_ = true;
            if (attached(n)) patch_log_.valid = false;
        }
        else if (method == "setPointerCapture" || method == "releasePointerCapture") {
            // The pointer id as event_json gives it out: the mouse is 1, a finger its pointer + 2.
            int64_t pointer_id = op["args"][size_t(0)].asInt();
            for (auto& [pointer, t] : touches_) {
                if ((t.mouse ? 1 : pointer + 2) != pointer_id) continue;
                if (method == "setPointerCapture" && container_->element(n->id)) set_capture(t, n->id, out);
                else if (t.capture == n->id) set_capture(t, 0, out);
            }
        }
        // A first report follows observe(), as in browsers.
        else if (method == "observeResize") resize_watch_.emplace(n->id, std::pair<float, float>{-1.0f, -1.0f});
        else if (method == "unobserveResize") resize_watch_.erase(n->id);
        else if (method == "focus") {
            if (now_s() - last_input_ < SCRIPT_FOCUS_S && text_control(n->id) == n->id) focus(n->id, out);
        } else if (method == "blur") {
            if (editing_.id == n->id) blur(out);
        } else if ((method == "select" || method == "setSelectionRange") && editing_.id == n->id) {
            int length = 0;
            for (unsigned char c : editing_.text) length += (c & 0xC0) != 0x80 ? ((c & 0xF8) == 0xF0 ? 2 : 1) : 0;
            const Json& args = op["args"];
            int start = method == "select" ? 0 : std::clamp(args[size_t(0)].asInt(0), 0, length);
            int end = method == "select" ? length : std::clamp(args[size_t(1)].asInt(start), start, length);
            editing_.selection_start = start;
            editing_.selection_end = end;
            editing_.generation = ++editor_generation_;
        }
        // scroll*: nothing to do without scrolling
    } else if (kind == "canvas") {
        if (n->tag == "canvas" && !n->svg) {
            // The document shows a canvas through its src, which names its size: a new or resized
            // bitmap changes the document.
            auto before = canvases_.find(n->id);
            int w = before == canvases_.end() ? -1 : before->second->width(), h = before == canvases_.end() ? -1 : before->second->height();
            if (Canvas* canvas = canvas_for(n)) {
                run_canvas(*canvas, op["cmds"]);
                if (canvas->width() != w || canvas->height() != h) {
                    document_changed_ = true;
                    note_attribute(n, "src");
                }
            }
        }
    }
}

void Renderer::adopt(int64_t id, Node* n, const Json& attrs) {
    if (n->id != 0 && n->id != id) aliases_.erase(n->id);
    n->id = id;
    // a non-owning alias: the fixed nodes live in html_node_/head_node_/body_node_
    nodes_.erase(id);
    aliases_[id] = n;
    for (const Json& pair : attrs.arr()) {
        if (!pair[0].isString() || !pair[1].isString() || starts_with_on(pair[0].asString())) continue;
        if (pair[0].asString() == "style") {
            set_inline_style(n->styles, pair[1].asString());
            continue;
        }
        bool found = false;
        for (auto& a : n->attrs) if (a.first == pair[0].asString()) { a.second = pair[1].asString(); found = true; }
        if (!found) n->attrs.emplace_back(pair[0].asString(), pair[1].asString());
    }
}

void Renderer::detach(Node* n) {
    if (!n->parent) return;
    auto& ch = n->parent->children;
    ch.erase(std::remove(ch.begin(), ch.end(), n), ch.end());
    n->parent = nullptr;
}

void Renderer::forget(Node* n) {
    forget_canvas(n->id);
    resize_watch_.erase(n->id);
    for (Node* c : n->children) {
        c->parent = nullptr;
        forget(c);
    }
    n->children.clear();
    nodes_.erase(n->id);  // deletes n
}

// ───────────────────────── serialisation ─────────────────────────

bool Renderer::attribute_holes(const Node* n) const {
    static const std::set<std::string> plain = {"html", "head", "body", "title", "meta", "link", "style", "base", "noscript"};
    return n->kind == Node::Element && n->id && !plain.count(n->tag) && n != html_node_.get() && n != head_node_.get() && n != body_node_.get();
}

void Renderer::serialize_node(const Node* n, std::string& out) const {
    switch (n->kind) {
    case Node::Comment: return;
    case Node::Text: escape_text(n->data, out); return;
    case Node::Element: break;
    }
    if (n->tag == "script" || n->tag == "template") return;  // never forwarded by the validator; belt and braces
    if (!css::is_tag_name(n->tag)) return;
    out.push_back('<');
    out += n->tag;
    if (n->id) {
        out += " data-twid=\"";
        out += std::to_string(n->id);
        out.push_back('"');
    }
    // The fork lays a <canvas> out as an image: its source is the backend's bitmap.
    const Canvas* canvas = nullptr;
    if (n->tag == "canvas" && !n->svg) {
        if (auto it = canvases_.find(n->id); it != canvases_.end()) canvas = it->second.get();
    }
    if (canvas) out += " src=\"tw-canvas:" + std::to_string(canvas->id()) + "/" + std::to_string(canvas->width()) + "x" + std::to_string(canvas->height()) + "\"";
    bool edited = n->id && n->id == editing_.id;
    // Every element a script can reach carries a style and (in HTML) a class attribute, empty or not, so that the
    // fork's template has a hole for each from the start: the first style or class a script gives an element (a row a
    // frame loop shows for the first time) is then a patch, not a new document. Not the document's own elements.
    const bool holes = attribute_holes(n);
    bool styled = !n->styles.empty() || overrides_.count(n->id) || edited || holes;
    bool classed = false;
    for (const auto& [name, value] : n->attrs) {
        if (name == "style") { styled = true; continue; }
        if (name == "class") classed = true;
        // A canvas's width and height are its natural size, which `src` names: on the fork's image they would be its style.
        if ((canvas && (name == "src" || name == "width" || name == "height")) || !css::is_attribute_name(name)) continue;
        // The platform's editor shows the placeholder of the control being edited.
        if (edited && name == "placeholder") continue;
        out.push_back(' ');
        out += name;
        out += "=\"";
        escape_attr(value, out);
        out.push_back('"');
    }
    if (holes && !classed && !n->svg) out += " class=\"\"";
    if (styled) {
        out += " style=\"";
        write_style(n, out);
        out.push_back('"');
    }
    out.push_back('>');
    if (!n->svg && is_void(n->tag)) return;
    if (n->tag == "style") {
        std::string css = css::resolve_light_dark(rewrite_color_scheme(n->data, dark_), dark_);
        size_t p = 0;
        while ((p = css.find("</", p)) != std::string::npos) { css.replace(p, 2, "<\\/"); p += 3; }
        out += css;
    } else if (n->tag == "textarea") {
        // The parser drops a newline right after <textarea>: a value that starts with one needs a second.
        if (!n->data.empty() && n->data[0] == '\n') out += '\n';
        escape_text(n->data, out);
        for (const Node* c : n->children) serialize_node(c, out);
    } else {
        for (const Node* c : n->children) serialize_node(c, out);
    }
    if (n == head_node_.get()) write_pseudo_overrides(out);
    out += "</";
    out += n->tag;
    out.push_back('>');
}

void Renderer::write_pseudo_overrides(std::string& out) const {
    // ::before and ::after have no style attribute: what their animations sample goes into a sheet of its
    // own, last and important, after the marker that keeps it out of what the scan reads as declared.
    std::string css;
    for (auto it = overrides_.begin(); it != overrides_.end() && it->first < 0; ++it) {
        std::string rule;
        for (const auto& [name, value] : it->second)
            if (css::is_property_name(name)) rule += ";" + name + ":" + css::resolve_light_dark(value, dark_) + " !important";
        if (rule.empty()) continue;
        css += "[data-twid=\"" + std::to_string(pseudo_owner(it->first)) + (pseudo_after(it->first) ? "\"]::after{" : "\"]::before{") + "-lh-anim:1" + rule + "}";
    }
    if (css.empty()) return;
    size_t p = 0;
    while ((p = css.find("</", p)) != std::string::npos) { css.replace(p, 2, "<\\/"); p += 3; }
    out += "<style>" + css + "</style>";
}

void Renderer::write_style(const Node* n, std::string& out) const {
    // Built plain, escaped once: the same text is also handed to the fork's style holes, which must
    // match what this writes into the HTML byte for byte.
    std::string plain;
    auto declare = [&](const std::string& name, const std::string& value) {
        if (!plain.empty() && plain.back() != ';') plain.push_back(';');
        plain += name;
        plain.push_back(':');
        plain += css::resolve_light_dark(value, dark_);
    };
    for (const auto& [name, value] : n->styles)
        if (css::is_property_name(name)) declare(name, value);
    // Last and important: neither the widget's stylesheets nor its own style on the root replace them.
    if (n == html_node_.get())
        for (const auto& [name, value] : theme_vars_) declare(name, value + " !important");
    auto it = n->id ? overrides_.find(n->id) : overrides_.end();
    bool edited = n->id && n->id == editing_.id;
    if (it != overrides_.end() || edited) {
        // The fork records nothing after this marker as declared, so animated values never feed back
        // into the next scan as the element's own.
        declare("-lh-anim", "1");
        if (it != overrides_.end())
            for (const auto& [name, value] : it->second)
                if (css::is_property_name(name)) declare(name, value);
        // The platform's editor shows the text of the control being edited.
        if (edited) declare("color", "transparent");
    }
    escape_attr(plain, out);
}

namespace {
const char* const kFrozenBadgeCss =
    "[data-tw-frozen=fatal] body{opacity:.85}[data-tw-badge]{position:fixed;right:6px;bottom:6px;z-index:2147483647;"
    "font:11px/1.3 -apple-system,system-ui,sans-serif;color:#fff;background:rgba(60,60,60,.75);padding:2px 7px;"
    "border-radius:9px}";
}

std::string Renderer::serialize() const {
    std::string out;
    out.reserve(4096);
    out += "<!DOCTYPE html>";
    if (layout_stopped_) {
        // Past the layout budget nothing of the widget's is laid out again, its stylesheets neither: the badge alone.
        out += "<html data-tw-frozen=\"fatal\"><head><style>" + std::string(kFrozenBadgeCss) +
               "</style></head><body><div data-tw-badge=\"fatal\">";
        escape_text(frozen_reason_, out);
        out += "</div></body></html>";
        return out;
    }
    // the frozen chrome: html attribute, badge style in head, badge in body (renderer elements, never in the widget's table)
    const Node* html = html_node_.get();
    out += "<html";
    if (html->id) out += " data-twid=\"" + std::to_string(html->id) + "\"";
    if (frozen_) out += frozen_fatal_ ? " data-tw-frozen=\"fatal\"" : " data-tw-frozen=\"paused\"";
    bool styled = !html->styles.empty() || !theme_vars_.empty() || overrides_.count(html->id);
    for (const auto& [name, value] : html->attrs) {
        if (name == "style") { styled = true; continue; }
        if (!css::is_attribute_name(name)) continue;
        out += " " + name + "=\"";
        escape_attr(value, out);
        out.push_back('"');
    }
    if (styled) {
        out += " style=\"";
        write_style(html, out);
        out.push_back('"');
    }
    out.push_back('>');
    for (const Node* c : html->children) {
        if (c == body_node_.get() && frozen_) {
            // badge before the body's own children would shift them; append it inside instead
            std::string body;
            serialize_node(c, body);
            size_t close = body.rfind("</body>");
            if (close != std::string::npos) {
                std::string badge = "<div data-tw-badge=\"" + std::string(frozen_fatal_ ? "fatal" : "paused") + "\">";
                escape_text(frozen_reason_, badge);
                badge += "</div>";
                body.insert(close, badge);
            }
            out += body;
        } else if (c == head_node_.get() && frozen_) {
            std::string head;
            serialize_node(c, head);
            size_t close = head.rfind("</head>");
            if (close != std::string::npos) head.insert(close, std::string("<style>") + kFrozenBadgeCss + "</style>");
            out += head;
        } else {
            serialize_node(c, out);
        }
    }
    out += "</html>";
    return out;
}

void Renderer::settle(std::vector<Json>& out) {
    if (!stale_) return;
    rebuild(true);
    report_size(out);
}

void Renderer::rebuild(bool patch) {
    Meter meter(*this);
    struct Refresh {
        Renderer& r;
        ~Refresh() { r.refresh_editor(); }
    } refresh{*this};
    stale_ = false;
    // Scripted layers follow what the build declares, which the composition key does not cover.
    if (!scripted_candidates_.empty()) composition_key_ = 0;
    note_before_change(patch);
    build_document(patch);
    paint_scan_ = true;
    animate();
    before_change_.clear();
}

void Renderer::note_before_change(bool patch) {
    (void)patch;
#ifdef TW_LITEHTML_TWIDGET
    before_change_.clear();
    std::map<int64_t, bool> restyled = std::move(restyled_);
    restyled_.clear();
    if (restyled.empty()) return;
    // Only where a transition can start: a sheet that declares one, or a restyled element's own style that does.
    bool possible = sheets_transition_;
    for (auto it = restyled.begin(); !possible && it != restyled.end(); ++it)
        if (const Node* n = node(it->first))
            for (const auto& [name, value] : n->styles) possible = possible || css::starts_with(name, "transition");
    if (!possible) return;
    // The elements this change restyles, as they are now: what a transition declared in the same change starts from
    // (its before-change style). A new style attribute restyles its element; a class or another attribute its
    // descendants too (selectors). Elements the Animator already watches have their old values there.
    std::vector<int64_t> alone, subtrees;
    for (const auto& [id, subtree] : restyled) (subtree ? subtrees : alone).push_back(id);
    auto skip = [this](int64_t id) { return animator_.knows(id); };
    before_change_ = container_->snapshot(Animator::watched(), alone, subtrees, skip);
#endif
}

void Renderer::charge(double spent) {
    double now = now_s();
    if (first_charge_ < 0) first_charge_ = now;
    if (spent <= 0 || now - first_charge_ < LOAD_GRACE_S) return;
    charges_.emplace_back(now, spent);
    while (!charges_.empty() && now - charges_.front().first > SUSTAINED_WINDOW_S) charges_.pop_front();
    if (spent <= THROTTLE_FREE_S) return;
    double total = 0;
    int heavy = 0;
    for (const auto& [at, cost] : charges_) {
        total += cost;
        heavy += cost > HEAVY_PASS_S;
    }
    double factor = total > SUSTAINED_LIMIT * SUSTAINED_WINDOW_S && heavy >= 2 ? SUSTAINED_THROTTLE_FACTOR : THROTTLE_FACTOR;
    throttled_until_ = std::max(throttled_until_, now) + spent * factor;
    stats_.throttled++;
}

bool Renderer::throttled() const {
    return now_s() < throttled_until_ && now_s() - last_input_ > INPUT_IMMEDIATE_S;
}

void Renderer::build_document(bool patch) {
    scripted_pending_ = false;
    PatchLog log = std::move(patch_log_);
    patch_log_ = PatchLog{};
#ifdef TW_LITEHTML_TWIDGET
    if (patch && patching_ && !layout_stopped_) {
        patch_log_ = std::move(log);
        bool patched = patch_document();
        patch_log_ = PatchLog{};
        if (patched && !layout_stopped_) return;
    }
#else
    (void)patch;
#endif
    double t0 = now_ms();
    html_ = serialize();
    html_stale_ = false;
    built_pseudo_.clear();
    for (auto it = overrides_.begin(); it != overrides_.end() && it->first < 0; ++it) built_pseudo_.insert(*it);
    double t1 = now_ms();
    stats_.serialize_ms = t1 - t0;
    stats_.html_bytes = html_.size();
    stats_.parse_ms = 0;
    stats_.passes = 0;
    // Like the web iframe: the viewport follows the document height (100vh must not explode), converging in a few passes.
    bool changed = false;
    for (int pass = 0; pass < MAX_PASSES; pass++) {
        double a = now_ms();
        changed = container_->build(html_, vp_height_) || changed;
        if (layout_stopped_) break;
        double b = now_ms();
        stats_.parse_ms += b - a;  // litehtml lays out inside createFromString+render; not separable
        stats_.passes++;
        if (pass == 0 && container_->patched()) stats_.patched++;
        int h = int(std::ceil(std::clamp(container_->height(), MIN_HEIGHT, max_height_)));
        if (h == vp_height_) break;
        bool again = container_->depends_on_viewport_height(html_);
        vp_height_ = h;
        if (!again) {
            // Nothing lays out against the viewport's height: the new one only needs recording.
            container_->viewport_resized(vp_height_);
            break;
        }
    }
    if (container_->exhausted()) {
        // Past the layout budget (stop_layout): the document as far as it got is no picture of the widget. Built
        // once more, from scratch, as the badge alone (serialize).
        container_->drop();
        html_ = serialize();
        container_->build(html_, vp_height_);
        vp_height_ = int(std::ceil(std::clamp(container_->height(), MIN_HEIGHT, max_height_)));
        container_->viewport_resized(vp_height_);
        changed = true;
    }
    doc_height_ = std::ceil(container_->height());
    stats_.builds++;
    if (changed) {
        dirty_ = true;
        touched_all_ = true;
    }
}

bool Renderer::attached(const Node* n) const {
    for (; n; n = n->parent)
        if (n == html_node_.get()) return true;
    return false;
}

void Renderer::note_attribute(const Node* n, const std::string& name) {
    // What the next rebuild restyles (note_before_change), whether or not it can patch.
    if (n->kind == Node::Element && n->id) {
        bool& subtree = restyled_[n->id];
        subtree = subtree || name != "style";
    }
    if (!patch_log_.valid || !attached(n)) return;
    // The attributes the Swift renderer patches; anything else, or a text node, builds.
    // `hidden` toggles as a boolean attribute through the element's class hole (a list re-sorting on a frame loop
    // hides and shows rows: each was a whole new document).
    if (n->kind != Node::Element || !(name == "class" || name == "style" || name == "src" || name == "hidden" || n->svg)) {
        patch_log_.valid = false;
        return;
    }
    patch_log_.attributes[n].insert(name);
}

void Renderer::note_children(const Node* parent, const Node* child) {
    if (!patch_log_.valid || !parent || !attached(parent)) return;
    if (child->kind != Node::Text) {
        patch_log_.valid = false;
        return;
    }
    patch_log_.texts.insert(parent);
}

bool Renderer::patch_document() {
#ifdef TW_LITEHTML_TWIDGET
    const PatchLog& log = patch_log_;
    if (!log.valid || (log.attributes.empty() && log.texts.empty()) || !container_->can_patch()) return false;
    double t0 = now_ms();
    std::vector<litehtml::document::hole_change> changes;
    for (const auto& [n, names] : log.attributes) {
        auto el = container_->element(n->id);
        if (!el) return false;
        for (const std::string& name : names) {
            litehtml::document::hole_change change;
            change.owner = el;
            change.name = name;
            // Byte for byte what serialize_node writes there.
            if (name == "style") {
                if (n->styles.empty() && !overrides_.count(n->id) && n->id != editing_.id && !attribute_holes(n)) return false;
                write_style(n, change.value);
            } else if (name == "hidden") {
                change.attribute = true;
                change.present = std::any_of(n->attrs.begin(), n->attrs.end(), [](const auto& p) { return p.first == "hidden"; });
            } else if (name == "src" && n->tag == "canvas" && !n->svg) {
                auto it = canvases_.find(n->id);
                if (it == canvases_.end()) return false;
                const Canvas& c = *it->second;
                change.value = "tw-canvas:" + std::to_string(c.id()) + "/" + std::to_string(c.width()) + "x" + std::to_string(c.height());
            } else {
                auto a = std::find_if(n->attrs.begin(), n->attrs.end(), [&](const auto& p) { return p.first == name; });
                // A class removed is the empty class serialize_node writes for such an element.
                bool empty_class = a == n->attrs.end() && name == "class" && !n->svg && attribute_holes(n);
                if ((a == n->attrs.end() && !empty_class) || !css::is_attribute_name(name)) return false;
                if (a != n->attrs.end()) escape_attr(a->second, change.value);
            }
            changes.push_back(std::move(change));
        }
    }
    for (const Node* n : log.texts) {
        auto el = container_->element(n->id);
        if (!el || n->tag == "style" || n->tag == "script") return false;
        litehtml::document::hole_change change;
        change.owner = el;
        change.text = true;
        if (n->tag == "textarea") {
            if (!n->children.empty() || n->data.empty()) return false;
            if (n->data[0] == '\n') change.value.push_back('\n');
            escape_text(n->data, change.value);
        } else {
            // A hole holds an element's text when the text is all it holds.
            if (n->children.size() != 1 || n->children[0]->kind != Node::Text || n->children[0]->data.empty()) return false;
            escape_text(n->children[0]->data, change.value);
        }
        changes.push_back(std::move(change));
    }
    if (!container_->patch(changes)) return false;
    int h = int(std::ceil(std::clamp(container_->height(), MIN_HEIGHT, max_height_)));
    // A new height moves the viewport: when the layout depends on it, the full path settles that, with
    // the patch already in. Style sheets only change through that path, so html_'s are current.
    if (h != vp_height_) {
        if (container_->depends_on_viewport_height(html_)) return false;
        vp_height_ = h;
        container_->viewport_resized(h);
    }
    doc_height_ = std::ceil(container_->height());
    stats_.parse_ms = now_ms() - t0;
    stats_.serialize_ms = 0;
    stats_.passes = 1;
    for (const auto& [n, names] : log.attributes) touched_.insert(n->id);
    for (const Node* n : log.texts) touched_.insert(n->id);
    stats_.builds++;
    stats_.patched++;
    stats_.in_place++;
    dirty_ = true;
    html_stale_ = true;
    return true;
#else
    return false;
#endif
}

// ───────────────────────── animations ─────────────────────────

void Renderer::animate() {
#ifdef TW_LITEHTML_TWIDGET
    if (animating_ || still()) return;
    animating_ = true;
    double now = now_s();
    // The sheets' raw text decides: they are resolved for the theme only when it changed.
    std::vector<const Node*> styles;
    std::string key(1, dark_ ? 'd' : 'l');
    std::function<void(const Node*)> visit = [&](const Node* n) {
        if (n->kind != Node::Element) return;
        if (n->tag == "style") {
            styles.push_back(n);
            key += n->data;
            key.push_back('\x01');
            return;
        }
        for (const Node* c : n->children) visit(c);
    };
    visit(html_node_.get());
    if (key != sheets_key_) {
        sheets_key_ = std::move(key);
        sheets_transition_ = sheets_key_.find("transition") != std::string::npos;
        std::vector<std::string> sheets;
        for (const Node* n : styles) sheets.push_back(css::resolve_light_dark(rewrite_color_scheme(n->data, dark_), dark_));
        animator_.set_style_sheets(sheets);
    }
    animator_.prune([this](int64_t id) { return node(pseudo_owner(id)) != nullptr; });
    uint64_t generation = 0;
    const auto& found = container_->declarations(Animator::watched(), Animator::entry_properties(), {}, &generation);
    auto targets = animator_.web_targets();
    if (targets.empty()) {
        // The same declarations as last time start, stop and retarget nothing.
        if (generation == 0 || generation != declared_generation_) {
            animator_.underlying.clear();
            animator_.update(found, now, before_change_.empty() ? nullptr : &before_change_, [this](int64_t id) { return container_->displayed(id); },
                            [this](int64_t id, const std::string&) { return container_->parent_color(id); });
        }
        declared_generation_ = generation;
    } else {
        declared_generation_ = 0;
        auto declarations = found;
        animator_.underlying.clear();
        // Web Animations targets are read whatever they declare: their keyframes compose onto these values.
        std::vector<std::string> names = Animator::watched();
        for (int64_t id : targets)
            for (const std::string& p : animator_.web_properties(id))
                if (std::find(names.begin(), names.end(), p) == names.end()) names.push_back(p);
        for (auto& [id, values] : container_->declarations(names, {}, targets)) {
            auto& declared = declarations[id];
            for (const auto& [k, v] : values)
                if (!css::starts_with(k, "animation") && !css::starts_with(k, "transition")) declared.emplace(k, v);
            animator_.underlying[id] = std::move(values);
        }
        animator_.update(declarations, now, before_change_.empty() ? nullptr : &before_change_, [this](int64_t id) { return container_->displayed(id); },
                            [this](int64_t id, const std::string&) { return container_->parent_color(id); });
    }
    // A keyframe that leaves out an inherited colour starts or ends at the one the element inherits.
    for (const auto& [id, property] : animator_.undeclared({"color"}))
        if (auto colour = container_->parent_color(id)) animator_.underlying[id][property] = *colour;
    update_composition(now);
    auto sampled = animator_.sample(now);
    strip_layers(sampled);
    queue_animation_events();
    // Changed overrides patch the style attributes of the document just built; only a change the
    // patch cannot take builds again.
    apply_overrides(std::move(sampled));
    animating_ = false;
    count_paint();
#endif
}

void Renderer::apply_overrides(StyleOverrides sampled) {
    // The ::before / ::after values the document's sheet holds, when a frame patched them since (a restyle of their
    // owner reads the sheet again): the sheet catches up once they stop moving.
    auto pseudo_patched = [this] {
        for (auto it = overrides_.begin(); it != overrides_.end() && it->first < 0; ++it) {
            auto b = built_pseudo_.find(it->first);
            if (b == built_pseudo_.end() || b->second != it->second) return true;
        }
        return false;
    };
    if (sampled == overrides_) {
        if (pseudo_patched()) build_document();
        return;
    }
    std::vector<int64_t> changed;
    for (const auto& [id, values] : sampled) {
        auto it = overrides_.find(id);
        if (it == overrides_.end() || it->second != values) changed.push_back(id);
    }
    for (const auto& [id, values] : overrides_)
        if (!sampled.count(id)) changed.push_back(id);
    overrides_ = std::move(sampled);
#ifdef TW_LITEHTML_TWIDGET
    // An animation frame changes style attributes only: patch them in place instead of serialising
    // the whole tree. Falls back to a build when an element has no style attribute yet, or the
    // document height moved.
    std::vector<std::pair<litehtml::element::ptr, std::string>> patches;
    bool ok = true;
    // ::before and ::after have their sampled values in the sheet (write_pseudo_overrides): a new sheet is a new
    // document. When the boxes and properties are the ones the sheet already animates (a sweep rotating every frame),
    // their paint values patch the boxes in place, all of them (a restyle since may have read the sheet's again).
    bool pseudo_changed = false;
    for (int64_t id : changed) pseudo_changed = pseudo_changed || id < 0;
    if (pseudo_changed) {
        auto names = [](const StyleValues& values) {
            std::vector<std::string> out;
            for (const auto& [name, value] : values) out.push_back(name);
            std::sort(out.begin(), out.end());
            return out;
        };
        std::vector<std::pair<int64_t, std::string>> generated;
        size_t count = 0;
        for (auto it = overrides_.begin(); ok && it != overrides_.end() && it->first < 0; ++it) {
            ++count;
            auto b = built_pseudo_.find(it->first);
            ok = b != built_pseudo_.end() && names(b->second) == names(it->second);
            if (!ok) break;
            std::string declarations;
            for (const auto& [name, value] : it->second) declarations += name + ":" + css::resolve_light_dark(value, dark_) + ";";
            generated.emplace_back(it->first, std::move(declarations));
        }
        ok = ok && count == built_pseudo_.size();
        for (const auto& [id, declarations] : generated) {
            if (!ok) break;
            ok = container_->patch_generated(id, declarations);
            // Its owner's layer (and the layers it is in) paint it.
            touched_.insert(pseudo_owner(id));
        }
    }
    for (int64_t id : changed) {
        if (id < 0) continue;
        const Node* n = node(id);
        auto el = container_->element(id);
        if (!n || !el) {
            ok = false;
            break;
        }
        std::string style;
        write_style(n, style);
        patches.emplace_back(std::move(el), std::move(style));
    }
    double t0 = now_ms();
    if (ok && (patches.empty() || container_->restyle(patches)) && !layout_stopped_) {
        int h = int(std::ceil(std::clamp(container_->height(), MIN_HEIGHT, max_height_)));
        if (h == vp_height_) {
            doc_height_ = std::ceil(container_->height());
            stats_.parse_ms = now_ms() - t0;
            stats_.serialize_ms = 0;
            stats_.passes = 1;
            stats_.builds++;
            stats_.patched++;
            stats_.in_place++;
            dirty_ = true;
            html_stale_ = true;
            touched_.insert(changed.begin(), changed.end());
            return;
        }
    }
#endif
    build_document();
}

void Renderer::queue_animation_events() {
    for (const AnimationEvent& e : animator_.take_events()) {
        if (frozen_ || !listened(e.type.c_str())) continue;
        Json ev = Json::object();
        // A pseudo-element's events are its owner's, naming it (AnimationEvent.pseudoElement).
        ev.set("type", e.type).set("targetId", double(pseudo_owner(e.target)));
        if (e.target < 0) ev.set("pseudoElement", pseudo_after(e.target) ? "::after" : "::before");
        ev.set(e.type == "transitionend" ? "propertyName" : "animationName", e.name);
        ev.set("elapsedTime", e.elapsed);
        pending_events_.push_back(event_message(std::move(ev)));
    }
}

std::vector<Json> Renderer::tick() {
    std::vector<Json> out;
    check_hold(out);
    // Held back, a frame only hands out the coalesced moves; the build and the animation step wait.
    if (throttled()) {
        flush_moves(out);
        flush_pending(out);
        return out;
    }
    Meter meter(*this);
    settle(out);
    flush_moves(out);
    double now = now_s();
    if (!still() && (animator_.active(now) || now >= wake_at_)) {
        update_composition(now);
        auto sampled = animator_.sample(now);
        strip_layers(sampled);
        queue_animation_events();
        apply_overrides(std::move(sampled));
        count_paint();
        report_size(out);
    }
    flush_pending(out);
    return out;
}

bool Renderer::wants_tick() const {
    auto hold = hold_deadline();
    return !pending_events_.empty() || !pending_violations_.empty() || frame_due() || (hold && now_ms() >= *hold);
}

bool Renderer::frame_due() const { return !still() && (stale_ || !pending_moves_.empty() || animator_.active(now_s(), layer_ids_)); }

double Renderer::next_tick_in() const {
    if (!pending_events_.empty() || !pending_violations_.empty()) return 0;
    if (still()) return std::numeric_limits<double>::infinity();
    double hold = std::numeric_limits<double>::infinity();
    if (auto at = hold_deadline()) hold = std::max(0.0, (*at - now_ms()) / 1000);
    if (stale_) return std::min(hold, std::max(0.0, throttled_until_ - now_s()));
    return std::min(hold, std::max(0.0, wake_at_ - now_s()));
}

// ───────────────────────── compositor layers ─────────────────────────

void Renderer::set_compositing(bool enabled) {
    if (enabled == compositing_) return;
    compositing_ = enabled;
    composition_key_ = 0;
    rebuild();
}

void Renderer::strip_layers(StyleOverrides& sampled) const {
    for (int64_t id : inert_) sampled.erase(id);
    for (const auto& [id, p] : promoted_) {
        auto it = sampled.find(id);
        if (it == sampled.end()) continue;
        auto& values = it->second;
        bool shimmer = p.shimmer;
        // A layer of a CSS animation plays its left / top and the size of a plain fill too (update_composition).
        bool sized = !p.scripted && !p.svg && !p.canvas && !p.svg_child;
        bool shadowless = p.shadowless;
        values.erase(std::remove_if(values.begin(), values.end(),
                                    [&](const auto& v) {
                                        if (shimmer) return is_shimmer_property(v.first);
                                        return is_layer_property(v.first) || (shadowless && v.first == "box-shadow") ||
                                               (sized && (is_offset_property(v.first) || v.first == "height" || v.first == "width"));
                                    }),
                     values.end());
        if (values.empty()) sampled.erase(it);
    }
}

void Renderer::update_composition(double now) {
    wake_at_ = std::numeric_limits<double>::infinity();
    auto drop_all = [&] {
        inert_.clear();
        if (promoted_.empty()) return;
        promoted_.clear();
        layer_ids_.clear();
        composition_key_ = 0;
        dirty_ = true;
    };
#ifdef TW_LITEHTML_TWIDGET
    if (!compositing_ || still()) return drop_all();
    struct Want {
        litehtml::element::ptr el;
        Animator::Plan plan;
        Rect box;
        uint64_t signature;
    };
    std::vector<std::pair<int64_t, Want>> wanted;
    bool iterations = interest_.count("animationiteration") > 0;
    auto ids = animator_.animated_ids(now);
    // An element whose animations touch no property (a transition that settled, an empty keyframe set)
    // animates nothing: it must not keep a still neighbour from becoming a layer.
    ids.erase(std::remove_if(ids.begin(), ids.end(), [&](int64_t id) { return animator_.animated_properties(id).empty(); }), ids.end());
    // Inert: an element of an SVG drawing that draws nothing whatever it animates (raindrops under a stroke
    // with alpha 0): its frames change no pixel. They are checked again whenever the layers are.
    std::unordered_set<int64_t> inert;
    for (int64_t id : ids) {
        const Node* n = node(id);
        if (!n || !svg_inside(n)) continue;
        auto el = container_->element(id);
        if (el && !container_->svg_draws_something(el)) inert.insert(id);
    }
    ids.erase(std::remove_if(ids.begin(), ids.end(), [&](int64_t id) { return inert.count(id) > 0; }), ids.end());
    // What decides the layers: the layout, the animations and where their plans stand. The same as
    // last time, nothing below can come out differently, so a frame stops here.
    struct Planned {
        int64_t id;
        Animator::Plan plan;
        uint64_t signature;
    };
    std::vector<Planned> planned;
    uint64_t key = container_->layouts() * 0x9E3779B97F4A7C15ull ^ std::hash<double>()(width_);
    for (int64_t id : ids) {
        auto plan = animator_.plan(id, now, iterations);
        if (!plan) continue;
        wake_at_ = std::min(wake_at_, plan->wake);
        bool usable = plan->ready && (plan->repeat || plan->duration >= MIN_LAYER_SPAN);
        uint64_t signature = animator_.signature(id);
        key = (key * 1099511628211ull) ^ uint64_t(id) ^ (signature << 2) ^ uint64_t(plan->repeat) ^ (uint64_t(usable) << 1);
        if (usable) planned.push_back({id, *plan, signature});
    }
    for (int64_t id : inert) key = (key * 1099511628211ull) ^ uint64_t(id) ^ 0x1E27ull;
    if (key == composition_key_ && composition_key_ != 0) return;
    composition_key_ = key;
    inert_ = inert;
    // Why each animated element is or is not a layer (composition_debug), decided below.
    why_.clear();
    auto why = [&](int64_t id, std::string reason) { why_.try_emplace(id, std::move(reason)); };
    for (int64_t id : ids) {
        auto plan = animator_.plan(id, now, iterations);
        if (!plan) why(id, "timing cannot be baked");
        else if (!(plan->ready && (plan->repeat || plan->duration >= MIN_LAYER_SPAN))) why(id, plan->ready ? "shorter than a layer is worth" : "waits out a delay");
    }

    // A layer holds a still picture: nothing inside or around it may animate, but for an ancestor
    // that only repaints its own background. One walk up from each animated element marks the
    // elements with something animated inside.
    std::unordered_set<int64_t> animated(ids.begin(), ids.end());
    // Movers: elements whose usable animations touch only layer properties. One inside another
    // becomes a layer inside the other's (nested), so neither rules the other out.
    // What a layer can play: the layer properties, left / top of a box they only move, and the height or
    // width of a plain fill they only stretch (with opacity at most).
    auto playable = [&](int64_t id, const std::vector<std::string>& properties) {
        bool left = false, top = false, moves = false;
        std::string size;
        for (const std::string& name : properties) {
            if (is_layer_property(name)) moves = moves || name != "opacity";
            else if (name == "left") left = true;
            else if (name == "top") top = true;
            else if ((name == "height" || name == "width") && size.empty()) size = name;
            else return false;
        }
        if (!left && !top && size.empty()) return true;
        auto el = container_->element(id);
        if (!el) return false;
        if (!size.empty()) {
            float anchor, ref;
            return !moves && !left && !top && container_->size_frame(el, size == "height", anchor, ref) &&
                   css::parse_translate(animator_.base_value(id, size), ref, ref).value_or(Affine{}).e > 0 && container_->plain_fill(el);
        }
        float w, h;
        return container_->offset_frame(el, left, top, w, h) && (!left || css::parse_translate(animator_.base_value(id, "left"), w, h)) &&
               (!top || css::parse_translate("0 " + animator_.base_value(id, "top"), w, h));
    };
    std::unordered_set<int64_t> movers;
    std::unordered_set<const litehtml::element*> mover_elements;
    for (const Planned& p : planned) {
        auto properties = animator_.animated_properties(p.id);
        const Node* n = node(pseudo_owner(p.id));
        if (!n || svg_inside(n) || !playable(p.id, properties)) continue;
        movers.insert(p.id);
        if (auto el = container_->element(p.id)) mover_elements.insert(el.get());
    }
    std::unordered_set<int64_t> holds_animated;
    for (int64_t id : ids) {
        if (movers.count(id)) continue;
        for (const Node* p = parent_node(id); p; p = p->parent)
            if (p->id && !holds_animated.insert(p->id).second) break;
    }
    std::unordered_map<int64_t, bool> local_background;
    auto blocks = [&](int64_t ancestor) {
        if (!animated.count(ancestor)) return false;
        auto [it, fresh] = local_background.try_emplace(ancestor, false);
        if (fresh) {
            auto properties = animator_.animated_properties(ancestor);
            it->second = properties.size() == 1 && properties[0] == "background-color" && container_->background_is_local(ancestor);
        }
        return !it->second;
    };
    std::vector<const Planned*> shimmers;
    std::vector<const Planned*> rings;
    std::vector<const Planned*> svg_children;
    for (const Planned& p : planned) {
        int64_t id = p.id;
        auto properties = animator_.animated_properties(id);
        if (!playable(id, properties)) {
            if (!properties.empty() && std::all_of(properties.begin(), properties.end(), [](const std::string& name) { return is_shimmer_property(name); }))
                shimmers.push_back(&p);
            if (id > 0 && properties.size() == 1 && properties[0] == "box-shadow") rings.push_back(&p);
            std::string list;
            for (const auto& name : properties) list += (list.empty() ? "" : ",") + name;
            why(id, "animates " + list);
            continue;
        }
        // A ::before or ::after (a negative id) is a leaf inside its owner.
        const Node* n = node(pseudo_owner(id));
        if (!n) continue;
        // An outer <svg> is a replaced box of the HTML layout like an <img>: its CSS transform is the box's.
        // An element inside the drawing becomes a layer of its own kind (svg_children below).
        if (id > 0 && svg_inside(n)) {
            svg_children.push_back(&p);
            continue;
        }
        if (holds_animated.count(id)) { why(id, "something animated inside"); continue; }
        bool blocked = false;
        int64_t blocker = 0;
        for (const Node* a = parent_node(id); a && !blocked; a = a->parent) {
            blocked = a->id && !movers.count(a->id) && blocks(a->id);
            if (blocked) blocker = a->id;
        }
        if (blocked) { why(id, "ancestor " + std::to_string(blocker) + " animates"); continue; }
        auto el = container_->element(id);
        Rect box;
        if (!el || !container_->layer_candidate(el, box, &mover_elements)) { why(id, el ? container_->candidate_why(el, &mover_elements) : "no element"); continue; }
        wanted.emplace_back(id, Want{std::move(el), p.plan, box, p.signature});
    }
    for (auto it = scripted_candidates_.begin(); it != scripted_candidates_.end();)
        it = node(*it) ? std::next(it) : scripted_candidates_.erase(it);
    for (auto it = svg_layers_.begin(); it != svg_layers_.end();)
        it = node(*it) ? std::next(it) : svg_layers_.erase(it);
    if (wanted.empty() && scripted_candidates_.empty() && shimmers.empty() && rings.empty() && svg_layers_.empty() && canvas_layers_.empty() && svg_children.empty()) {
        if (!promoted_.empty()) {
            promoted_.clear();
            composition_only_ = false;
            dirty_ = true;
        }
        layer_ids_ = inert_;
        for (int64_t id : inert_) why_[id] = "inert (draws nothing)";
        return;
    }

    std::map<int64_t, Promotion> next;
    std::unordered_map<int64_t, litehtml::element::ptr> elements;
    for (auto& [id, want] : wanted) {
        Promotion p;
        p.box = want.box;
        p.signature = want.signature;
        if (!container_->clip_of(want.el, p.has_clip, p.clip, p.clip_radii)) { why(id, "clipped by two ancestors"); continue; }
        p.found_clip = p.has_clip;
        p.found_clip_rect = p.clip;
        // A running track stays when nothing about it changed, and while it still covers what the
        // animations will do: the end of a one-shot track, or the same loop.
        auto old = promoted_.find(id);
        bool same_span = old != promoted_.end() &&
                         (want.plan.repeat ? old->second.track.repeat && std::fabs(old->second.track.duration - want.plan.duration) < 1e-6
                                           : !old->second.track.repeat && std::fabs(old->second.track.start + old->second.track.duration - (want.plan.start + want.plan.duration)) < 1e-3);
        if (same_span && old->second.signature == want.signature && std::memcmp(&old->second.box, &want.box, sizeof(Rect)) == 0) {
            p.track = old->second.track;
            p.track_version = old->second.track_version;
        } else {
            const Rect& box = want.box;
            auto origin = css::parse_origin(animator_.base_value(id, "transform-origin"), box.w, box.h);
            if (!origin) { why(id, "transform-origin"); continue; }
            Affine to = Affine::translate(box.x + origin->first, box.y + origin->second);
            Affine from = Affine::translate(-(box.x + origin->first), -(box.y + origin->second));
            const Animator::Plan& plan = want.plan;
            int64_t target = id;
            // left / top: the box is laid out at the declared values; the track moves it by the difference.
            float cbw = 0, cbh = 0;
            auto animated = animator_.animated_properties(id);
            bool left = std::find(animated.begin(), animated.end(), "left") != animated.end();
            bool top = std::find(animated.begin(), animated.end(), "top") != animated.end();
            if ((left || top) && !container_->offset_frame(want.el, left, top, cbw, cbh)) { why(id, "left / top"); continue; }
            auto offset = [&](const std::string& l, const std::string& t) -> std::optional<Affine> {
                auto x = left ? css::parse_translate(l, cbw, cbh) : std::optional<Affine>(Affine{});
                auto y = top ? css::parse_translate("0 " + t, cbw, cbh) : std::optional<Affine>(Affine{});
                if (!x || !y) return std::nullopt;
                return Affine::translate(x->e, y->f);
            };
            auto base_offset = offset(animator_.base_value(id, "left"), animator_.base_value(id, "top"));
            if (!base_offset) { why(id, "left / top"); continue; }
            // height / width of a plain fill: a stretch about the edge that stays (size_frame).
            std::string size = std::find(animated.begin(), animated.end(), "height") != animated.end() ? "height"
                               : std::find(animated.begin(), animated.end(), "width") != animated.end() ? "width" : "";
            float anchor = 0, ref = 0, base_size = 0;
            if (!size.empty()) {
                if (!container_->size_frame(want.el, size == "height", anchor, ref)) { why(id, size); continue; }
                base_size = css::parse_translate(animator_.base_value(id, size), ref, ref).value_or(Affine{}).e;
                if (!(base_size > 0)) { why(id, size); continue; }
            }
            auto stretch = [&](const std::string& value) -> std::optional<Affine> {
                if (size.empty()) return Affine{};
                auto v = css::parse_translate(value, ref, ref);
                if (!v) return std::nullopt;
                float k = std::max(0.0f, v->e) / base_size;
                if (size == "height") {
                    float y = box.y + anchor * box.h;
                    return Affine::translate(0, y) * Affine{1, 0, 0, k, 0, 0} * Affine::translate(0, -y);
                }
                float x = box.x + anchor * box.w;
                return Affine::translate(x, 0) * Affine{k, 0, 0, 1, 0, 0} * Affine::translate(-x, 0);
            };
            auto sample = [&](double t, LayerKey& out) {
                StyleValues values = animator_.peek(target, plan_time(plan, t));
                auto value = [&](const char* property) {
                    for (auto it = values.rbegin(); it != values.rend(); ++it)
                        if (it->first == property) return it->second;
                    return animator_.base_value(target, property);
                };
                auto translate = css::parse_translate(value("translate"), box.w, box.h);
                auto rotate = css::parse_rotate(value("rotate"));
                auto scale = css::parse_scale(value("scale"));
                auto transform = css::parse_transform(value("transform"), box.w, box.h);
                auto opacity = css::parse_opacity(value("opacity"));
                auto moved = offset(value("left"), value("top"));
                auto stretched = stretch(size.empty() ? std::string() : value(size.c_str()));
                if (!translate || !rotate || !scale || !transform || !opacity || !moved || !stretched) return false;
                out.matrix = Affine::translate(moved->e - base_offset->e, moved->f - base_offset->f) * to * *translate * *rotate * *scale * *transform * from * *stretched;
                out.opacity = *opacity;
                return true;
            };
            p.track.start = plan.start;
            p.track.duration = plan.duration;
            p.track.repeat = plan.repeat;
            if (!bake_track(sample, box, p.track.keys)) { why(id, "track does not bake"); continue; }
            p.track_version = next_version_++;
        }
        // What the layer can cover, and how fine to rasterise it.
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f, scale = 1;
        for (const LayerKey& k : p.track.keys) {
            const float corners[4][2] = {{p.box.x, p.box.y}, {p.box.x + p.box.w, p.box.y}, {p.box.x, p.box.y + p.box.h}, {p.box.x + p.box.w, p.box.y + p.box.h}};
            for (const auto& c : corners) {
                float x = k.matrix.apply_x(c[0], c[1]), y = k.matrix.apply_y(c[0], c[1]);
                x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
            }
            scale = std::max(scale, std::sqrt(std::fabs(k.matrix.a * k.matrix.d - k.matrix.b * k.matrix.c)));
        }
        p.raster_scale = std::min(scale, 3.0f);
        p.reach = Rect{x0, y0, x1 - x0, y1 - y0};
        // A clip the whole track stays inside, away from its corners, cuts nothing: the layer goes without.
        if (p.has_clip) {
            const Radii& q = p.clip_radii;
            float corner = std::max({q.tlx, q.tly, q.trx, q.try_, q.brx, q.bry, q.blx, q.bly});
            if (p.reach.x >= p.clip.x + corner && p.reach.y >= p.clip.y + corner && p.reach.x + p.reach.w <= p.clip.x + p.clip.w - corner &&
                p.reach.y + p.reach.h <= p.clip.y + p.clip.h - corner)
                p.has_clip = false;
        }
        if (p.has_clip) {
            float cx0 = std::max(p.reach.x, p.clip.x), cy0 = std::max(p.reach.y, p.clip.y);
            float cx1 = std::min(p.reach.x + p.reach.w, p.clip.x + p.clip.w), cy1 = std::min(p.reach.y + p.reach.h, p.clip.y + p.clip.h);
            p.reach = Rect{cx0, cy0, std::max(0.0f, cx1 - cx0), std::max(0.0f, cy1 - cy0)};
        }
        elements[id] = want.el;
        next.emplace(id, std::move(p));
    }

    // Elements of an SVG drawing moving on their own (a spinning sun, drifting clouds): their own transform
    // and opacity as a track in document coordinates, outer * own(t) * outer^-1, the content drawn without
    // them, clipped by the <svg>'s box. Nothing else of the drawing animates around them.
    for (const Planned* planned_child : svg_children) {
        int64_t id = planned_child->id;
        const Node* n = node(id);
        auto el = container_->element(id);
        auto root = el ? Container::svg_root_of(el) : nullptr;
        if (!n || !el || !root) { why(id, "SVG child outside a drawing"); continue; }
        int64_t root_id = 0;
        bool still = true;
        for (const Node* a = n->parent; a; a = a->parent) {
            if (a->id && (animated.count(a->id) || movers.count(a->id))) still = false;
            if (a->tag == "svg" && !(a->parent && a->parent->svg)) {
                root_id = a->id;
                break;
            }
        }
        if (!still || !root_id) { why(id, "SVG child under an animated element"); continue; }
        bool blocked = false;
        for (const Node* a = node(root_id)->parent; a && !blocked; a = a->parent) blocked = a->id && blocks(a->id);
        Promotion p;
        if (blocked || !container_->layer_candidate(root, p.box)) { why(id, blocked ? "SVG child: ancestor animates" : "SVG child: its <svg> " + container_->candidate_why(root)); continue; }
        // The layer's clip is the <svg>'s box inside whatever clips the <svg>.
        bool outer_clip = false;
        Rect outer_rect;
        Radii outer_radii;
        if (!container_->clip_of(root, outer_clip, outer_rect, outer_radii)) { why(id, "clipped by two ancestors"); continue; }
        p.has_clip = true;
        p.clip = outer_clip ? intersection(p.box, outer_rect) : p.box;
        if (outer_clip && std::memcmp(&p.clip, &outer_rect, sizeof(Rect)) == 0) p.clip_radii = outer_radii;
        p.found_clip = true;
        p.found_clip_rect = p.clip;
        p.signature = planned_child->signature ^ 0x5C61D0ull;
        // The shape where it is without its own transform: its transform-box origin moves with it, so a new cx / cy
        // (a chart's end dot moved by a script) bakes the track again instead of keeping the old one's origin.
        litehtml::transform_matrix frame_outer;
        litehtml::position frame_ref;
        float frame_ox = 0, frame_oy = 0;
        std::optional<Rect> shape;
        if (litehtml::svg_own_frame(el, frame_outer, frame_ref, frame_ox, frame_oy)) {
            litehtml::vector_path geometry;
            float x0, y0, x1, y1;
            if (litehtml::svg_geometry(el, geometry) && path_bounds(geometry, x0, y0, x1, y1))
                shape = bounds_under(Rect{x0, y0, x1 - x0, y1 - y0}, Affine{frame_outer.a, frame_outer.b, frame_outer.c, frame_outer.d, frame_outer.tx, frame_outer.ty});
            for (float v : {frame_ox, frame_oy, float(frame_ref.x), float(frame_ref.y), float(frame_ref.width), float(frame_ref.height)}) {
                uint32_t bits;
                std::memcpy(&bits, &v, sizeof bits);
                p.signature = (p.signature ^ bits) * 0x100000001B3ull;
            }
        }
        const Animator::Plan& plan = planned_child->plan;
        auto old = promoted_.find(id);
        bool same_span = old != promoted_.end() &&
                         (plan.repeat ? old->second.track.repeat && std::fabs(old->second.track.duration - plan.duration) < 1e-6
                                      : !old->second.track.repeat && std::fabs(old->second.track.start + old->second.track.duration - (plan.start + plan.duration)) < 1e-3);
        if (same_span && old->second.signature == p.signature && std::memcmp(&old->second.box, &p.box, sizeof(Rect)) == 0) {
            p.track = old->second.track;
            p.track_version = old->second.track_version;
        } else {
            litehtml::transform_matrix outer;
            litehtml::position ref;
            float ox = 0, oy = 0;
            if (!litehtml::svg_own_frame(el, outer, ref, ox, oy)) { why(id, "SVG child: no frame"); continue; }
            Affine m{outer.a, outer.b, outer.c, outer.d, outer.tx, outer.ty}, inverse;
            if (!m.invert(inverse)) { why(id, "SVG child: flat frame"); continue; }
            float attr_opacity = 1;
            for (const auto& [k, v] : n->attrs)
                if (k == "opacity") attr_opacity = std::clamp(std::strtof(v.c_str(), nullptr), 0.0f, 1.0f);
            float rw = ref.width.value(), rh = ref.height.value();
            auto sample = [&](double t, LayerKey& out) {
                StyleValues values = animator_.peek(id, plan_time(plan, t));
                auto value = [&](const char* property) {
                    for (auto it = values.rbegin(); it != values.rend(); ++it)
                        if (it->first == property) return it->second;
                    return animator_.base_value(id, property);
                };
                auto translate = css::parse_translate(value("translate"), rw, rh);
                auto rotate = css::parse_rotate(value("rotate"));
                auto scale = css::parse_scale(value("scale"));
                auto transform = css::parse_transform(value("transform"), rw, rh);
                auto opacity = css::parse_opacity(value("opacity"));
                if (!translate || !rotate || !scale || !transform || !opacity) return false;
                Affine own = Affine::translate(ox, oy) * *translate * *rotate * *scale * *transform * Affine::translate(-ox, -oy);
                out.matrix = m * own * inverse;
                out.opacity = *opacity * attr_opacity;
                return true;
            };
            p.track.start = plan.start;
            p.track.duration = plan.duration;
            p.track.repeat = plan.repeat;
            if (!bake_track(sample, p.box, p.track.keys)) { why(id, "track does not bake"); continue; }
            p.track_version = next_version_++;
        }
        float scale = 1;
        for (const LayerKey& k : p.track.keys) scale = std::max(scale, std::sqrt(std::fabs(k.matrix.a * k.matrix.d - k.matrix.b * k.matrix.c)));
        p.raster_scale = std::min(scale, 3.0f);
        // The layer goes above the whole document, so above the shapes the drawing paints after it too: one that
        // reaches where it moves keeps it in the document (Chromium draws the later shape on top: the dot over its
        // pulse). Where it moves: its shape under each key, a little wider for a stroke; the <svg>'s box when the
        // shape has no geometry of its own (a group).
        Rect moves = p.clip;
        if (shape) {
            float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
            for (const LayerKey& k : p.track.keys) {
                Rect b = bounds_under(*shape, k.matrix);
                x0 = std::min(x0, b.x); y0 = std::min(y0, b.y); x1 = std::max(x1, b.x + b.w); y1 = std::max(y1, b.y + b.h);
            }
            if (std::isfinite(x0)) moves = intersection(Rect{x0 - 2, y0 - 2, x1 - x0 + 4, y1 - y0 + 4}, p.clip);
        }
        bool under = false;
        std::function<void(const Node*)> later = [&](const Node* c) {
            if (under || c->kind != Node::Element) return;
            if (auto e = c->id ? container_->element(c->id) : nullptr) {
                if (auto b = svg_box_of(e); b && !b->empty() && !intersection(*b, moves).empty()) under = true;
            }
            for (const Node* g : c->children) later(g);
        };
        for (const Node* at = n; at && at->id != root_id && at->parent && !under; at = at->parent) {
            const auto& siblings = at->parent->children;
            for (auto it = std::find(siblings.begin(), siblings.end(), at); it != siblings.end() && ++it != siblings.end() && !under;) later(*it);
        }
        if (under) { why(id, "SVG child: a later shape draws over it"); continue; }
        p.reach = p.clip;
        p.svg_child = true;
        // Coverage and paint order are the <svg>'s.
        elements[id] = root;
        next.emplace(id, std::move(p));
    }

    // Shimmers (the Swift renderer's WidgetShimmers): a background-position animation of a lone
    // gradient tile is the tile sliding under the element's clip, baked like any other track.
    for (const Planned* planned_shimmer : shimmers) {
        int64_t id = planned_shimmer->id;
        const Animator::Plan& plan = planned_shimmer->plan;
        const Node* n = node(id);
        if (!n || !n->children.empty() || next.count(id) || holds_animated.count(id)) continue;
        bool blocked = false;
        for (const Node* a = n->parent; a && !blocked; a = a->parent) blocked = a->id && blocks(a->id);
        auto el = container_->element(id);
        Promotion p;
        Rect area;
        if (blocked || !el || !container_->background_tile(el, p.box, area, p.clip, p.clip_radii, p.tile)) continue;
        // An ancestor's clip must not cut into the tile's: a layer carries one.
        bool outer = false;
        Rect outer_clip;
        Radii outer_radii;
        if (!container_->clip_of(el, outer, outer_clip, outer_radii)) continue;
        if (outer) {
            float corner = std::max({outer_radii.tlx, outer_radii.tly, outer_radii.trx, outer_radii.try_, outer_radii.brx, outer_radii.bry, outer_radii.blx, outer_radii.bly});
            if (!contains(Rect{outer_clip.x + corner, outer_clip.y + corner, outer_clip.w - 2 * corner, outer_clip.h - 2 * corner}, p.clip)) continue;
        }
        p.shimmer = true;
        p.has_clip = true;
        p.signature = planned_shimmer->signature;
        p.tile_version = paint_digest(p.tile);
        auto old = promoted_.find(id);
        bool same_span = old != promoted_.end() && old->second.shimmer &&
                         (plan.repeat ? old->second.track.repeat && std::fabs(old->second.track.duration - plan.duration) < 1e-6
                                      : !old->second.track.repeat && std::fabs(old->second.track.start + old->second.track.duration - (plan.start + plan.duration)) < 1e-3);
        if (same_span && old->second.signature == p.signature && std::memcmp(&old->second.box, &p.box, sizeof(Rect)) == 0) {
            p.track = old->second.track;
            p.track_version = old->second.track_version;
        } else {
            std::string base = animator_.base_value(id, "background-position");
            auto sample = [&](double t, LayerKey& out) {
                StyleValues values = animator_.peek(id, plan_time(plan, t));
                std::optional<float> x;
                bool found = false;
                for (auto it = values.rbegin(); it != values.rend() && !found; ++it)
                    if (is_shimmer_property(it->first)) {
                        found = true;
                        x = shimmer_x(it->second, area, p.box);
                    }
                if (!found) x = shimmer_x(base.empty() ? "0% 0%" : base, area, p.box);
                if (!x) return false;
                out.matrix = Affine::translate(*x - p.box.x, 0);
                out.opacity = 1;
                return true;
            };
            p.track.start = plan.start;
            p.track.duration = plan.duration;
            p.track.repeat = plan.repeat;
            if (!bake_track(sample, p.box, p.track.keys)) continue;
            p.track_version = next_version_++;
        }
        p.reach = p.clip;
        elements[id] = el;
        next.emplace(id, std::move(p));
    }

    // A box-shadow pulse (a live dot): a round box whose one outer shadow grows (spread) and fades, never
    // offset or blurred. A spread shadow of a circle is a bigger circle: the shadow at its widest, a disc
    // in the shadow's colour, scaled about the centre and faded by a track, makes every frame of it. It
    // goes under a still layer of the element without its shadows; the document leaves the element out.
    for (const Planned* planned_ring : rings) {
        int64_t id = planned_ring->id;
        const Animator::Plan& plan = planned_ring->plan;
        const Node* n = node(pseudo_owner(id));
        auto el = container_->element(id);
        // What decides comes after "animates box-shadow" in composition_debug.
        auto refuse = [&](const char* reason) { why_[id] = std::string("animates box-shadow; ring: ") + reason; };
        if (!n || !el || next.count(id) || holds_animated.count(id) || !el->children().empty()) { refuse("children or something animated inside"); continue; }
        bool blocked = false;
        for (const Node* a = parent_node(id); a && !blocked; a = a->parent) blocked = a->id && blocks(a->id);
        Promotion dot;
        if (blocked || !container_->layer_candidate(el, dot.box) || !container_->clip_of(el, dot.has_clip, dot.clip, dot.clip_radii)) { refuse("not a layer candidate"); continue; }
        const Rect box = dot.box;
        // Round, and a plain opaque fill under the shadow.
        Rect bounds;
        DisplayList own = container_->draw_layer(el, {}, bounds);
        bool plain = !own.empty() && std::abs(box.w - box.h) < 0.5f;
        for (const Command& c : own) {
            if (c.kind == Cmd::Shadow) continue;
            plain = plain && c.kind == Cmd::FillRect && c.color.a == 255 && c.radii.tlx >= box.w / 2 - 0.5f && c.radii.tly >= box.h / 2 - 0.5f &&
                    c.radii.brx >= box.w / 2 - 0.5f && c.radii.bry >= box.h / 2 - 0.5f;
        }
        if (!plain) { refuse("not a plain round box"); continue; }
        // The widest the shadow gets, and its colour.
        float widest = 0, strongest = 0;
        Color colour;
        bool ok = true;
        for (int i = 0; i <= 32 && ok; i++) {
            StyleValues values = animator_.peek(id, plan_time(plan, i / 32.0));
            std::string text = animator_.base_value(id, "box-shadow");
            for (auto it = values.rbegin(); it != values.rend(); ++it)
                if (it->first == "box-shadow") { text = it->second; break; }
            auto sh = parse_shadow(text);
            ok = sh && sh->x == 0 && sh->y == 0 && sh->blur == 0 && sh->spread >= 0 && (i == 0 || (sh->color.r == colour.r && sh->color.g == colour.g && sh->color.b == colour.b));
            if (!ok) break;
            if (i == 0) colour = sh->color;
            widest = std::max(widest, sh->spread);
            strongest = std::max(strongest, float(sh->color.a));
        }
        if (!ok || widest <= 0 || strongest <= 0) { refuse("not one growing, unblurred, centred shadow"); continue; }
        float r = box.w / 2, cx = box.x + r, cy = box.y + r, outer = r + widest;
        Promotion ring;
        ring.ring = true;
        ring.box = Rect{cx - outer, cy - outer, outer * 2, outer * 2};
        ring.has_clip = dot.has_clip;
        ring.clip = dot.clip;
        ring.clip_radii = dot.clip_radii;
        Command disc;
        disc.kind = Cmd::FillRect;
        disc.rect = ring.box;
        disc.radii = Radii{outer, outer, outer, outer, outer, outer, outer, outer};
        disc.color = colour;
        disc.color.a = uint8_t(strongest);
        ring.tile = {disc};
        ring.tile_version = paint_digest(ring.tile);
        ring.signature = planned_ring->signature ^ 0x217Bull;
        auto sample = [&](double t, LayerKey& out) {
            StyleValues values = animator_.peek(id, plan_time(plan, t));
            std::string text = animator_.base_value(id, "box-shadow");
            for (auto it = values.rbegin(); it != values.rend(); ++it)
                if (it->first == "box-shadow") { text = it->second; break; }
            auto sh = parse_shadow(text);
            if (!sh) return false;
            float k = (r + sh->spread) / outer;
            out.matrix = Affine::translate(cx, cy) * Affine{k, 0, 0, k, 0, 0} * Affine::translate(-cx, -cy);
            out.opacity = float(sh->color.a) / strongest;
            return true;
        };
        auto old = promoted_.find(id | RING_BIT);
        bool same_span = old != promoted_.end() && old->second.ring && old->second.signature == ring.signature &&
                         std::memcmp(&old->second.box, &ring.box, sizeof(Rect)) == 0 && old->second.track.repeat == plan.repeat &&
                         std::fabs(old->second.track.duration - plan.duration) < 1e-6 && (plan.repeat || std::fabs(old->second.track.start - plan.start) < 1e-3);
        if (same_span) {
            ring.track = old->second.track;
            ring.track_version = old->second.track_version;
        } else {
            ring.track.start = plan.start;
            ring.track.duration = plan.duration;
            ring.track.repeat = plan.repeat;
            if (!bake_track(sample, ring.box, ring.track.keys)) { refuse("track does not bake"); continue; }
            ring.track_version = next_version_++;
        }
        ring.reach = bounds_under(ring.box, Affine{});
        if (ring.has_clip) ring.reach = intersection(ring.reach, ring.clip);
        dot.shadowless = true;
        dot.signature = planned_ring->signature ^ 0xD07ull;
        if (!still_track(id, dot, now)) { refuse("no still track"); continue; }
        elements[id] = el;
        elements[id | RING_BIT] = el;
        next.emplace(id, std::move(dot));
        next.emplace(id | RING_BIT, std::move(ring));
    }

    // Only a layer nothing paints over can go above the document. Dropping one can uncover
    // another's neighbour or put back what covered another, so this runs until nothing changes.
    const auto& trace = container_->paint_order();

    // Scripted layers: still pictures apply_scripted moves, each alone in its branch of the tree, with
    // room to move around where it is now and the raster scale its transform asks for.
    {
        std::unordered_set<const litehtml::element*> others;
        for (const auto& [id, p] : next) others.insert(elements[id].get());
        std::vector<std::pair<int64_t, litehtml::element::ptr>> candidates;
        for (int64_t id : scripted_candidates_) {
            const Node* n = node(id);
            if (next.count(id) || animated.count(id) || holds_animated.count(id) || animator_.declares_motion(id) || !n || n->svg || n->tag == "svg") continue;
            bool alone = true;
            for (const auto& [other, p] : next) alone = alone && !is_ancestor(other, id) && !is_ancestor(id, other);
            for (int64_t other : scripted_candidates_) alone = alone && (other == id || (!is_ancestor(other, id) && !is_ancestor(id, other)));
            for (const Node* a = n->parent; a && alone; a = a->parent) alone = !(a->id && blocks(a->id));
            auto el = container_->element(id);
            if (!alone || !el) continue;
            others.insert(el.get());
            candidates.emplace_back(id, std::move(el));
        }
        for (auto& [id, el] : candidates) {
            Promotion p;
            if (!container_->layer_candidate(el, p.box) || double(p.box.w) * p.box.h >= MAX_SCRIPTED_AREA || !container_->clip_of(el, p.has_clip, p.clip, p.clip_radii)) continue;
            p.scripted = true;
            p.base = container_->declared(id, {"transform", "translate", "rotate", "scale", "opacity", "transform-origin"});
            LayerKey key;
            if (!scripted_key(id, p, key)) continue;
            const Affine& m = key.matrix;
            bool translating = m.a == 1 && m.b == 0 && m.c == 0 && m.d == 1;
            bool rotated = std::fabs(m.b) > 1e-4f || std::fabs(m.c) > 1e-4f;
            float scale = std::sqrt(std::fabs(m.a * m.d - m.b * m.c));
            p.raster_scale = translating ? 1 : std::min(3.0f, std::max(rotated ? 2.0f : 1.0f, scale * 1.25f));
            p.signature = 0x5C819ull ^ uint64_t(id);
            auto old = promoted_.find(id);
            if (old != promoted_.end() && old->second.scripted && old->second.track.keys.size() == 1 && old->second.track.keys[0].matrix == m &&
                old->second.track.keys[0].opacity == key.opacity && std::memcmp(&old->second.box, &p.box, sizeof(Rect)) == 0) {
                p.track = old->second.track;
                p.track_version = old->second.track_version;
            } else {
                p.track.start = now;
                p.track.keys.assign(1, key);
                p.track_version = next_version_++;
            }
            // The widest margin around it that nothing paints over.
            Rect at = bounds_under(p.box, m);
            for (float margin : {std::max(48.0f, std::max(p.box.w, p.box.h) / 2), 16.0f, 2.0f}) {
                p.reach = Rect{at.x - margin, at.y - margin, at.w + 2 * margin, at.h + 2 * margin};
                if (p.has_clip) p.reach = intersection(p.reach, p.clip);
                if (!container_->covered(el, p.reach, trace, others)) break;
            }
            if (p.has_clip) {
                const Radii& q = p.clip_radii;
                float corner = std::max({q.tlx, q.tly, q.trx, q.try_, q.brx, q.bry, q.blx, q.bly});
                Rect inner{p.clip.x + corner, p.clip.y + corner, p.clip.w - 2 * corner, p.clip.h - 2 * corner};
                if (contains(inner, p.reach)) p.has_clip = false;
            }
            elements[id] = el;
            next.emplace(id, std::move(p));
        }
    }

    // SVG layers (the Swift renderer's WidgetSVGLayers): an SVG a script changes batch after batch
    // repaints alone, and the document around it does not.
    for (int64_t id : svg_layers_) {
        const Node* n = node(id);
        if (!n || next.count(id) || animated.count(id) || holds_animated.count(id)) continue;
        bool alone = true;
        for (const auto& [other, p] : next) alone = alone && !is_ancestor(other, id) && !is_ancestor(id, other);
        for (const Node* a = n->parent; a && alone; a = a->parent) alone = !(a->id && blocks(a->id));
        auto el = container_->element(id);
        Promotion p;
        if (!alone || !el || !container_->layer_candidate(el, p.box) || !container_->clip_of(el, p.has_clip, p.clip, p.clip_radii)) continue;
        p.svg = true;
        p.signature = 0x5F6ull ^ uint64_t(id);
        if (!still_track(id, p, now)) continue;
        elements[id] = el;
        next.emplace(id, std::move(p));
    }

    // Canvas layers: a canvas that keeps drawing repaints only itself.
    for (int64_t id : canvas_layers_) {
        const Node* n = node(id);
        if (!n || n->tag != "canvas" || next.count(id) || animated.count(id)) continue;
        bool alone = true;
        for (const auto& [other, p] : next) alone = alone && !is_ancestor(other, id) && !is_ancestor(id, other);
        for (const Node* a = n->parent; a && alone; a = a->parent) alone = !(a->id && blocks(a->id));
        auto el = container_->element(id);
        Promotion p;
        if (!alone || !el || !container_->layer_candidate(el, p.box) || !container_->clip_of(el, p.has_clip, p.clip, p.clip_radii)) continue;
        p.canvas = true;
        p.signature = 0xCA9ull ^ uint64_t(id);
        if (!still_track(id, p, now)) continue;
        elements[id] = el;
        next.emplace(id, std::move(p));
    }

    // A few static elements over a wanted layer (labels on a moving card) become layers of their own,
    // still ones, so the moving one can go under them.
    {
        std::unordered_set<const litehtml::element*> ignore_all;
        for (const auto& [id, p] : next) ignore_all.insert(elements[id].get());
        size_t budget = MAX_LAYERS > next.size() ? MAX_LAYERS - next.size() : 0;
        std::vector<int64_t> queue;
        for (const auto& [id, p] : next) queue.push_back(id);
        for (int64_t id : queue) {
            if (budget == 0) break;
            const Promotion& wanted_layer = next[id];
            if (wanted_layer.reach.empty() || !container_->covered(elements[id], wanted_layer.reach, trace, ignore_all)) continue;
            auto found = container_->obscurers(elements[id], wanted_layer.reach, trace, ignore_all, MAX_OBSCURERS);
            if (!found || found->empty()) continue;
            // A descendant of another obscurer goes in that one's layer (a <small> inside the <h3>).
            {
                std::vector<int64_t> tops;
                for (int64_t o : *found)
                    if (std::none_of(found->begin(), found->end(), [&](int64_t other) { return other != o && is_ancestor(other, o); })) tops.push_back(o);
                *found = std::move(tops);
            }
            if (found->size() > budget) continue;
            std::vector<std::pair<int64_t, Promotion>> still;
            for (int64_t other : *found) {
                const Node* n = node(pseudo_owner(other));
                auto el = container_->element(other);
                Promotion s;
                if (next.count(other) || animated.count(other) || !n || (n->svg && n->tag != "svg") || is_ancestor(other, id) || is_ancestor(id, other) || !el ||
                    !container_->layer_candidate(el, s.box) || !container_->clip_of(el, s.has_clip, s.clip, s.clip_radii)) {
                    still.clear();
                    break;
                }
                s.signature = 0x57A71Cull ^ uint64_t(other);
                if (!still_track(other, s, now)) {
                    still.clear();
                    break;
                }
                elements[other] = el;
                still.emplace_back(other, std::move(s));
            }
            for (auto& [other, s] : still) {
                ignore_all.insert(elements[other].get());
                next.emplace(other, std::move(s));
                budget--;
            }
        }
    }
    for (auto& [id, p] : next) {
        auto ri = elements[id]->get_render_item();
        p.order = size_t(std::find(trace.begin(), trace.end(), ri.get()) - trace.begin());
    }

    // Nested layers: a layer inside a mover moves with the mover's layer. Its keys and its own clip
    // are in the parent's content space; a clip it shares with the parent is the parent's. What
    // nothing may cover is where the parent's keys take its reach.
    for (auto& [id, p] : next) {
        p.parent = 0;
        for (const Node* a = parent_node(id); a && !p.parent; a = a->parent)
            if (a->id && movers.count(a->id)) p.parent = a->id;
    }
    {
        std::vector<std::pair<int, int64_t>> by_depth;
        for (const auto& [id, p] : next) {
            int depth = 0;
            for (int64_t up = p.parent; up && next.count(up) && depth < 64; up = next[up].parent) depth++;
            by_depth.emplace_back(depth, id);
        }
        std::sort(by_depth.begin(), by_depth.end());
        for (const auto& [depth, id] : by_depth) {
            Promotion& p = next[id];
            auto parent = next.find(p.parent);
            if (!p.parent || parent == next.end()) continue;
            if (p.has_clip && parent->second.found_clip && std::memcmp(&p.found_clip_rect, &parent->second.found_clip_rect, sizeof(Rect)) == 0) p.has_clip = false;
            Rect reach{};
            bool first = true;
            for (const LayerKey& k : parent->second.track.keys) {
                Rect at = bounds_under(p.reach, k.matrix);
                if (first) reach = at;
                else reach = Rect{std::min(reach.x, at.x), std::min(reach.y, at.y), std::max(reach.x + reach.w, at.x + at.w) - std::min(reach.x, at.x),
                                  std::max(reach.y + reach.h, at.y + at.h) - std::min(reach.y, at.y)};
                first = false;
            }
            if (parent->second.has_clip) reach = intersection(reach, parent->second.clip);
            p.reach = reach;
        }
    }
    // A child goes with its parent, and only when nothing else is composited between them.
    auto drop_orphans = [&](std::map<int64_t, Promotion>& set) {
        for (bool dropped = true; dropped;) {
            dropped = false;
            for (auto it = set.begin(); it != set.end();) {
                int64_t parent = it->second.parent;
                bool orphan = false;
                if (parent) {
                    auto up = set.find(parent);
                    orphan = up == set.end();
                    for (const auto& [other, q] : set) {
                        if (orphan) break;
                        orphan = other != parent && other != it->first && q.order > up->second.order && q.order < it->second.order && !is_ancestor(parent, other);
                    }
                }
                if (orphan) {
                    it = set.erase(it);
                    dropped = true;
                } else {
                    ++it;
                }
            }
        }
    };
    drop_orphans(next);
    // A ring and its element's still layer go together or not at all.
    auto drop_halves = [&](std::map<int64_t, Promotion>& set) {
        for (auto it = set.begin(); it != set.end();) {
            bool half = (it->second.ring && !set.count(it->first & ~RING_BIT)) || (it->second.shadowless && !set.count(it->first | RING_BIT));
            it = half ? set.erase(it) : std::next(it);
        }
    };
    drop_halves(next);
    size_t rounds = next.size() + 1;
    for (size_t round = 0; round < rounds && !next.empty(); round++) {
        std::unordered_set<const litehtml::element*> ignore;
        for (const auto& [id, p] : next) ignore.insert(elements[id].get());
        std::vector<std::pair<int64_t, double>> uncovered;
        double total = 0;
        for (const auto& [id, p] : next) {
            // An empty reach is a layer that shows nothing (scale(0)): nothing can cover it, and it must stay one,
            // or it would cover the layer under it again from the document.
            if (!p.reach.empty() && container_->covered(elements[id], p.reach, trace, ignore)) {
                auto by = container_->obscurers(elements[id], p.reach, trace, ignore, 8);
                std::string list;
                if (by)
                    for (int64_t o : *by) list += " " + std::to_string(o);
                why(id, "painted over by" + (by ? list : std::string(" >8 or unnamed")));
                continue;
            }
            double s = dpr_ * p.raster_scale;
            double cost = double(p.box.w + 2 * LAYER_MARGIN) * (p.box.h + 2 * LAYER_MARGIN) * s * s;
            uncovered.emplace_back(id, cost);
            total += cost;
        }
        // Over the budget, the topmost layers go first and the first that does not fit ends it: a
        // skipped layer would leave the ones under it composited out of order with it.
        bool all = uncovered.size() <= MAX_LAYERS && total <= MAX_LAYER_PIXELS;
        if (!all) std::sort(uncovered.begin(), uncovered.end(), [&](const auto& a, const auto& b) { return std::make_pair(next[a.first].order, a.first) > std::make_pair(next[b.first].order, b.first); });
        std::map<int64_t, Promotion> admitted;
        double pixels = 0;
        for (const auto& [id, cost] : uncovered) {
            if (!all && (admitted.size() >= MAX_LAYERS || pixels + cost > MAX_LAYER_PIXELS)) break;
            pixels += cost;
            admitted.emplace(id, std::move(next[id]));
        }
        drop_orphans(admitted);
        drop_halves(admitted);
        drop_halves(admitted);
        bool stable = admitted.size() == next.size();
        next = std::move(admitted);
        if (stable) break;
    }

    bool changed = next.size() != promoted_.size();
    for (const auto& [id, p] : next) {
        auto old = promoted_.find(id);
        changed = changed || old == promoted_.end() || old->second.track_version != p.track_version || old->second.order != p.order ||
                  old->second.parent != p.parent || std::memcmp(&old->second.box, &p.box, sizeof(Rect)) != 0;
    }
    promoted_ = std::move(next);
    layer_ids_.clear();
    for (const auto& [id, p] : promoted_) {
        layer_ids_.insert(id);
        why_[id] = p.scripted ? "layer (scripted)" : p.svg ? "layer (svg)" : p.canvas ? "layer (canvas)" : p.shimmer ? "layer (shimmer)" : p.svg_child ? "layer (svg child)" : p.ring ? "layer (box-shadow ring)" : p.shadowless ? "layer (under: its ring)" : "layer";
    }
    for (int64_t id : inert_) {
        layer_ids_.insert(id);
        why_[id] = "inert (draws nothing)";
    }
    if (changed) {
        composition_only_ = false;
        dirty_ = true;
    }
#else
    (void)now;
    drop_all();
#endif
}

std::string Renderer::composition_debug() const {
    std::string out = "[";
    bool first = true;
    for (const auto& [id, reason] : why_) {
        const Node* n = node(id);
        std::string props;
        const Node* owner = id > 0 && (id & RING_BIT) ? node(id & ~RING_BIT) : nullptr;
        std::string tag = n ? n->tag : owner ? owner->tag + " ring" : id < 0 ? (pseudo_after(id) ? "::after" : "::before") : "?";
        for (const auto& name : animator_.animated_properties(id)) props += (props.empty() ? "" : ",") + name;
        out += std::string(first ? "" : ",") + "[" + std::to_string(id) + "," + twidget::Json::quote(tag) + "," + (n && n->svg ? "1" : "0") + "," +
               twidget::Json::quote(props) + "," + twidget::Json::quote(reason) + "]";
        first = false;
    }
    return out + "]";
}

std::string Renderer::layout_debug_json() const {
    Json out = Json::object();
    out.set("w", Json(width_)).set("h", Json(doc_height_)).set("dpr", Json(dpr_)).set("clock", Json(now_s()));
    Json vars = Json::object();
    for (const auto& [k, v] : theme_vars_) vars.set(k, Json(v));
    out.set("vars", std::move(vars));
    Json els = Json::array(), texts = Json::array(), anims = Json::array();
    std::unordered_map<int64_t, std::string> paths;
    auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower((unsigned char)c));
        return s;
    };
#ifdef TW_LITEHTML_TWIDGET
    // The k-th run of an element's own text that is not all white space, fragment by fragment.
    auto text_runs_of = [&](const litehtml::element::ptr& el, const std::string& path) {
        int k = 0;
        bool open = false, blank = true;
        std::vector<litehtml::element::ptr> run;
        auto flush = [&]() {
            if (open && !blank) {
                for (const auto& t : run) {
                    auto parent = t->parent();
                    int handle = 0;
                    float descent = 0, letter = 0, word = 0;
                    bool metrics = parent && container_->font_info(uintptr_t(parent->css().get_font()), handle, descent, letter, word);
                    auto text = std::dynamic_pointer_cast<litehtml::el_text>(t);
                    t->for_each_render_item([&](const std::shared_ptr<litehtml::render_item>& ri) {
                        litehtml::position placed = ri->get_placement();
                        auto item = std::dynamic_pointer_cast<litehtml::render_text>(ri);
                        auto add = [&](const litehtml::position& b, const std::string& s) {
                            Json f = Json::array();
                            f.push(Json(path)).push(Json(double(k))).push(Json(double(b.x))).push(Json(double(b.y))).push(Json(double(b.width)))
                                .push(Json(double(b.height))).push(metrics ? Json(double(b.y + b.height) - descent) : Json()).push(Json(s));
                            texts.push(std::move(f));
                        };
                        if (item && !item->fragments.empty()) {
                            for (const auto& fragment : item->fragments) {
                                litehtml::position b = fragment->pos();
                                b.x += placed.x - ri->pos().x;
                                b.y += placed.y - ri->pos().y;
                                add(b, fragment->fragment_text);
                            }
                        } else if (text) {
                            add(placed, item && item->cut >= 0 ? text->display_text().substr(0, size_t(item->cut)) : text->display_text());
                        }
                    });
                }
                k++;
            }
            open = false;
            blank = true;
            run.clear();
        };
        for (const auto& c : el->children()) {
            if (c->is_text()) {
                open = true;
                // Blank as the browser side reads it (ASCII white space only): a run of &nbsp; is text.
                if (!c->is_white_space()) {
                    blank = false;
                } else {
                    std::string t;
                    c->get_text(t);
                    if (t.find_first_not_of(" \t\n\r\f") != std::string::npos) blank = false;
                }
                run.push_back(c);
            } else if (!dynamic_cast<const litehtml::el_comment*>(c.get())) {
                flush();
            }
        }
        flush();
    };
#endif
    // `hidden`: under a display: none element. A replaced element (an <svg>, an <img>) keeps a render item when its
    // display is none, so that alone does not say it is not laid out (it read as shown with a 0 x 0 box).
    std::function<void(const Node*, const std::string&, bool, bool)> walk = [&](const Node* n, const std::string& path, bool in_svg, bool hidden) {
        std::unordered_map<std::string, int> seen;
        for (const Node* c : n->children) {
            if (c->kind != Node::Element) continue;
            std::string tag = lower(c->tag);
            std::string p = path + ">" + tag + ":" + std::to_string(++seen[tag]);
            if (c->id) paths[c->id] = p;
            auto el = c->id ? container_->element(c->id) : nullptr;
            int flags = in_svg ? 1 : 0;
            Rect b;
            bool none = hidden || (el && el->css().get_display() == litehtml::display_none);
            if (in_svg && none) {
                // Inside a drawing a browser reports a hidden shape as a 0 x 0 box, never as not laid out.
            } else if (!el || none || (!el->get_render_item() && !in_svg)) {
                flags |= 2;
            } else {
                b = script_box_of(el);
                if (!inline_boxes_of(el).empty()) flags |= 4;
            }
            Json e = Json::array();
            e.push(Json(p)).push(Json(double(c->id))).push(Json(tag)).push(Json(double(b.x))).push(Json(double(b.y))).push(Json(double(b.w)))
                .push(Json(double(b.h))).push(Json(double(flags)));
            els.push(std::move(e));
#ifdef TW_LITEHTML_TWIDGET
            if (el && !in_svg && !(flags & 2) && tag != "svg") text_runs_of(el, p);
#endif
            walk(c, p, in_svg || tag == "svg", none);
        }
    };
    if (body_node_) {
        paths[body_node_->id] = "body";
        if (auto el = body_node_->id ? container_->element(body_node_->id) : nullptr) {
            Rect b = border_box_of(el);
            Json e = Json::array();
            e.push(Json("body")).push(Json(double(body_node_->id))).push(Json("body")).push(Json(double(b.x))).push(Json(double(b.y)))
                .push(Json(double(b.w))).push(Json(double(b.h))).push(Json(0.0));
            els.push(std::move(e));
#ifdef TW_LITEHTML_TWIDGET
            text_runs_of(el, "body");
#endif
        }
        walk(body_node_.get(), "body", false, false);
    }
    double now = now_s();
    for (int64_t id : animator_.sampled_ids(now)) {
        auto it = paths.find(id);
        if (it == paths.end()) continue;
        Json values = Json::object();
        for (const auto& [property, value] : animator_.peek(id, now)) values.set(property, Json(container_->resolve_vars(id, value)));
        Json a = Json::array();
        a.push(Json(it->second)).push(Json(double(id))).push(std::move(values));
        anims.push(std::move(a));
    }
    out.set("el", std::move(els)).set("tx", std::move(texts)).set("an", std::move(anims));
    // A test harness's marker on <html> (tool/align_prelude.js: its virtual clock has stopped).
    if (html_node_)
        for (const auto& [name, value] : html_node_->attrs)
            if (name == "data-tw-vclock") out.set("vclock", Json(value));
    return out.dump();
}

// ───────────────────────── canvas ─────────────────────────

Canvas* Renderer::canvas_for(Node* n) {
    if (auto it = canvases_.find(n->id); it != canvases_.end()) return it->second.get();
    if (canvases_.size() >= MAX_CANVASES) return nullptr;
    auto size = [&](const char* name, int fallback) {
        for (const auto& [k, v] : n->attrs) {
            std::string t = css::trim(v);
            if (k == name && !t.empty() && std::isdigit((unsigned char)t[0])) return std::atoi(t.c_str());
        }
        return fallback;
    };
    auto canvas = std::make_unique<Canvas>(int(n->id), size("width", 300), size("height", 150), static_cast<CanvasHost&>(*this));
    Canvas* raw = canvas.get();
    canvases_[n->id] = std::move(canvas);
    CanvasOp reset;
    reset.kind = CanvasOp::Reset;
    backend_.canvas_run(raw->id(), raw->width(), raw->height(), {reset});
    return raw;
}

void Renderer::run_canvas(Canvas& canvas, const Json& cmds) {
    // Fonts are held across one batch at most, so a widget cycling through font strings cannot grow this.
    if (canvas_fonts_.size() > 64) {
        for (const auto& [font_css, font] : canvas_fonts_) backend_.delete_font(font.first);
        canvas_fonts_.clear();
    }
    std::vector<CanvasOp> ops;
    canvas.run(cmds, ops);
    if (ops.empty()) return;
    backend_.canvas_run(canvas.id(), canvas.width(), canvas.height(), ops);
    canvases_drawn_.insert(canvas.id());
    uint64_t draws = ++canvas_draws_[canvas.id()];
    // Its first drawing makes it something that can cover a layer.
    if (draws == 1) composition_key_ = 0;
    // Drawn a third time, a canvas looks animated: the Swift renderer presents such a canvas in a layer.
    if (draws >= 3 && canvas_layers_.size() < MAX_LAYERS && canvas_layers_.insert(canvas.id()).second) composition_key_ = 0;
    bool clean = !dirty_ || composition_only_;
    dirty_ = true;
    auto p = promoted_.find(canvas.id());
    auto layer = std::find_if(composition_.layers.begin(), composition_.layers.end(), [&](const Layer& l) { return l.id == canvas.id(); });
    if (p == promoted_.end() || !p->second.canvas || layer == composition_.layers.end()) {
        composition_only_ = false;
        return;
    }
    // Only the layer shows what the canvas drew.
    layer->content_version = content_version(layer->content);
    composition_.version = next_version_++;
    composition_only_ = clean;
}

uint64_t Renderer::content_version(const DisplayList& content) const {
    uint64_t version = paint_digest(content);
    for (const Command& c : content)
        if (c.canvas) {
            auto it = canvas_draws_.find(c.canvas);
            version = (version ^ uint64_t(c.canvas) ^ (it == canvas_draws_.end() ? 0 : it->second << 20)) * 1099511628211ull;
        }
    return version;
}

void Renderer::forget_canvas(int64_t id) {
    auto it = canvases_.find(id);
    if (it == canvases_.end()) return;
    canvas_draws_.erase(it->second->id());
    canvas_layers_.erase(it->second->id());
    backend_.canvas_release(it->second->id());
    canvases_.erase(it);
}

int Renderer::canvas_font(const std::string& font, FontMetrics& metrics) {
    if (auto it = canvas_fonts_.find(font); it != canvas_fonts_.end()) {
        metrics = it->second.second;
        return it->second.first;
    }
    css::Font f = css::parse_font(font);
    int handle = backend_.create_font(f.family, f.size, f.weight, f.italic, {}, metrics);
    if (handle && metrics.em_ascent == 0 && metrics.em_descent == 0) set_em_box(metrics, metrics.ascent, metrics.descent, f.size);
    if (handle) canvas_fonts_.emplace(font, std::make_pair(handle, metrics));
    return handle;
}

float Renderer::canvas_text_width(int font, const std::string& text) { return backend_.text_width(font, text); }

Json Renderer::measure_text(const std::string& font, const std::string& text) {
    // In the font fillText draws it with: the canvas's, not a document's.
    if (canvas_fonts_.size() > 64) {
        for (const auto& [font_css, held] : canvas_fonts_) backend_.delete_font(held.first);
        canvas_fonts_.clear();
    }
    FontMetrics fm;
    int handle = canvas_font(font, fm);
    float width = handle ? backend_.text_width(handle, text) : 0.0f;
    Json o = Json::object();
    o.set("width", width);
    o.set("actualBoundingBoxLeft", 0).set("actualBoundingBoxRight", width);
    o.set("actualBoundingBoxAscent", fm.ascent * 0.8f).set("actualBoundingBoxDescent", fm.descent * 0.6f);
    o.set("fontBoundingBoxAscent", fm.ascent).set("fontBoundingBoxDescent", fm.descent);
    o.set("emHeightAscent", fm.em_ascent).set("emHeightDescent", fm.em_descent);
    o.set("hangingBaseline", fm.ascent * 0.8f).set("alphabeticBaseline", 0).set("ideographicBaseline", -fm.descent);
    return o;
}

bool Renderer::canvas_source(int64_t id, int& image, int& canvas, float& w, float& h) {
    if (auto it = canvases_.find(id); it != canvases_.end()) {
        canvas = it->second->id();
        w = float(it->second->width());
        h = float(it->second->height());
        return true;
    }
    const Node* n = node(id);
    if (!n || n->tag != "img") return false;
    for (const auto& [k, v] : n->attrs)
        if (k == "src" && v.rfind("data:", 0) == 0) return container_->image_of(v, image, w, h);
    return false;
}

int64_t Renderer::canvas_allowance(int id) {
    int64_t used = 0;
    for (const auto& [node_id, canvas] : canvases_)
        if (canvas->id() != id) used += canvas->pixels();
    return Canvas::MAX_PIXELS - used;
}

void Renderer::report_size(std::vector<Json>& out) {
    if (track_resize_) note_resize();
    // Every frame asks; the message goes out when a number moved.
    std::array<double, 4> size{width_, doc_height_, double(vp_height_), dpr_};
    if (size == last_size_) return;
    last_size_ = size;
    Json msg = Json::object();
    msg.set("type", "size").set("width", width_).set("height", doc_height_);
    msg.set("innerWidth", width_).set("innerHeight", vp_height_).set("dpr", dpr_);
    if (has_resize_ && resize_serial_ != resize_reported_) {
        resize_reported_ = resize_serial_;
        Json r = Json::object();
        r.set("oldHeight", resize_.old_height).set("newHeight", resize_.new_height);
        r.set("unchangedTop", resize_.unchanged_top).set("unchangedBottom", resize_.unchanged_bottom);
        if (resize_.has_pointer) r.set("pointerFrom", resize_.pointer_from).set("pointerTo", resize_.pointer_to);
        r.set("keepsBottom", resize_.keeps_bottom);
        msg.set("resize", std::move(r));
    }
    out.push_back(std::move(msg));
}

void Renderer::set_resize_tracking(bool enabled, bool prefers_bottom) {
    track_resize_ = enabled;
    prefers_bottom_ = prefers_bottom;
    if (!enabled) {
        edges_.clear();
        edges_layouts_ = 0;
    }
}

namespace {
/// How long a press decides the side to keep, and how close height steps come in one burst (a CSS
/// height transition), whose tiny or unclear steps keep the side the burst started with.
constexpr double RESIZE_PRESS_AGE = 1.5, RESIZE_BURST_GAP = 0.15;
/// Edges that moved less than this did not move.
constexpr double RESIZE_TOLERANCE = 0.5;

bool keeps_bottom(const Renderer::ResizeDiff& d, std::optional<double> press, bool prefers_bottom, std::optional<bool> previous) {
    double old_h = d.old_height, delta = d.new_height - d.old_height;
    if (previous && (std::fabs(delta) <= 1.5 || (d.unchanged_top >= old_h && d.unchanged_bottom >= old_h))) return *previous;
    if (d.has_pointer) {
        // Whichever side the pressed element stays closer to.
        double shift = d.pointer_to - d.pointer_from;
        if (previous && std::fabs(shift) < 1 && std::fabs(shift - delta) < 1) return *previous;
        return std::fabs(shift - delta) < std::fabs(shift);
    }
    if (press) {
        double y = std::clamp(*press, 0.0, old_h);
        if (y <= d.unchanged_top) return false;
        if (y >= old_h - d.unchanged_bottom) return true;
        return y >= old_h / 2;
    }
    return prefers_bottom || d.unchanged_bottom > d.unchanged_top;
}
} // namespace

void Renderer::note_resize() {
    uint64_t layouts = container_->layouts();
    if (layouts == edges_layouts_) return;
    std::unordered_map<int64_t, Edges> now;
    now.reserve(edges_.size());
    for (const auto& [id, el] : container_->elements()) {
        Rect r = border_box_of(el);
        if (r.w > 0 || r.h > 0) now.emplace(id, Edges{r.y, r.y + r.h});
    }
    double old_h = edges_height_, new_h = container_->height();
    if (edges_layouts_ && std::fabs(std::ceil(old_h) - std::ceil(new_h)) > RESIZE_TOLERANCE) {
        // Edges that moved, appeared or went bound the parts that did not, from the top and from the bottom.
        ResizeDiff d;
        d.old_height = old_h;
        d.new_height = new_h;
        double top = old_h, bottom = old_h;
        auto unstable_top = [&](double y) { top = std::min(top, std::max(y, 0.0)); };
        auto unstable_bottom = [&](double distance) { bottom = std::min(bottom, std::max(distance, 0.0)); };
        for (const auto& [id, before] : edges_) {
            auto after = now.find(id);
            if (after == now.end()) {
                unstable_top(before.top);
                unstable_bottom(old_h - before.bottom);
                continue;
            }
            for (auto [a, b] : {std::pair<double, double>{before.top, after->second.top}, {before.bottom, after->second.bottom}}) {
                if (std::fabs(a - b) > RESIZE_TOLERANCE) unstable_top(std::min(a, b));
                if (std::fabs((old_h - a) - (new_h - b)) > RESIZE_TOLERANCE) unstable_bottom(std::min(old_h - a, new_h - b));
            }
        }
        for (const auto& [id, after] : now) {
            if (edges_.count(id)) continue;
            unstable_top(after.top);
            unstable_bottom(new_h - after.bottom);
        }
        d.unchanged_top = std::min(top, old_h);
        d.unchanged_bottom = std::min(bottom, old_h);

        double t = now_s();
        bool recent = t - press_at_ < RESIZE_PRESS_AGE;
        auto before = edges_.find(press_id_);
        auto after = now.find(press_id_);
        if (recent && before != edges_.end() && after != now.end()) {
            double inset = std::clamp(press_y_ - before->second.top, 0.0,
                                      double(std::min(before->second.bottom - before->second.top, after->second.bottom - after->second.top)));
            d.has_pointer = true;
            d.pointer_from = before->second.top + inset;
            d.pointer_to = after->second.top + inset;
        }
        std::optional<bool> previous;
        if (has_resize_ && t - resize_at_ < RESIZE_BURST_GAP) previous = resize_.keeps_bottom;
        d.keeps_bottom = keeps_bottom(d, recent ? std::optional<double>(press_y_) : std::nullopt, prefers_bottom_, previous);
        // The press stays on the content it was on.
        if (d.keeps_bottom) press_y_ += new_h - old_h;
        resize_ = d;
        resize_at_ = t;
        has_resize_ = true;
        resize_serial_++;
    }
    edges_ = std::move(now);
    edges_height_ = new_h;
    edges_layouts_ = layouts;
}

DisplayList Renderer::paint() {
    Meter meter(*this);
    // Messages from a build here go out with the next ones.
    settle(pending_events_);
    if (reuse_paint()) {
        dirty_ = false;
        composition_only_ = false;
        damage_.clear();
        damage_valid_ = true;
        canvases_drawn_.clear();
        stats_.reused++;
        return painted_list_;
    }
    composition_only_ = false;
    // The paint finds the runs again, in the document or in a layer.
    if (selectable_text())
        for (const TextRun& r : text_runs()) r.drawn = false;
    DisplayList list = paint_document();
    drop_inert_clips(list);
    paint_selection(list, 0, true);
    // What changed against the last paint, for a platform to repaint only that.
    PaintSummary summary = summarize(list);
    damage_valid_ = painted_valid_ && painted_size_ == std::make_pair(px_width(), px_height()) &&
                    damage_between(painted_, summary, [this](int canvas) { return canvases_drawn_.count(canvas) != 0; }, damage_);
    painted_ = std::move(summary);
    painted_valid_ = true;
    painted_size_ = {px_width(), px_height()};
    canvases_drawn_.clear();
    painted_list_valid_ = std::any_of(promoted_.begin(), promoted_.end(), [](const auto& p) { return p.second.scripted || p.second.canvas; });
#ifdef TW_LITEHTML_TWIDGET
    if (painted_list_valid_) {
        painted_list_ = list;
        painted_key_ = container_->paint_key();
    } else {
        painted_list_ = DisplayList{};
    }
#endif
    return list;
}

bool Renderer::reuse_paint() const {
#ifdef TW_LITEHTML_TWIDGET
    if (!composition_only_ || !painted_list_valid_ || !painted_valid_ || !touched_.empty() || touched_all_) return false;
    for (int canvas : canvases_drawn_) {
        auto p = promoted_.find(canvas);
        if (p == promoted_.end() || !p->second.canvas) return false;
    }
    if (container_->paint_key() != painted_key_ || painted_size_ != std::make_pair(px_width(), px_height())) return false;
    if (promoted_.size() != composition_.layers.size()) return false;
    for (size_t i = 0; i < composition_.layers.size(); i++) {
        auto it = promoted_.find(composition_.layers[i].id);
        if (it == promoted_.end() || it->second.track_version != composition_.layers[i].track_version) return false;
        if (i > 0 && it->second.order <= promoted_.at(composition_.layers[i - 1].id).order) return false;
    }
    return true;
#else
    return false;
#endif
}

bool Renderer::scripted_only(const Json::Array& ops, std::vector<int64_t>& touched) const {
    if (still() || ops.empty() || promoted_.empty()) return false;
    for (const Json& op : ops) {
        if (op["op"].asString() != "style" || op["important"].asBool()) return false;
        // Taking a property away goes back to what the document declares, which only a build knows.
        const std::string& value = op["value"].asString();
        if (value.empty() || value.size() > 256 || !is_layer_property(lower(op["name"].asString()))) return false;
        int64_t id = int64_t(op["id"].asNumber());
        auto it = promoted_.find(id);
        if (it == promoted_.end() || !it->second.scripted) return false;
        if (std::find(touched.begin(), touched.end(), id) == touched.end()) touched.push_back(id);
    }
    return true;
}

void Renderer::note_scripted(const Json::Array& ops) {
    ops_batch_++;
    double now = now_s();
    std::vector<int64_t> moving;
    for (const Json& op : ops)
        if (op["op"].asString() == "style" && is_layer_property(lower(op["name"].asString()))) moving.push_back(int64_t(op["id"].asNumber()));
    if (moving.empty()) return;
    bool changed = false;
    if (moving.size() > MAX_LAYERS) {
        // A crowd moves together: layers would not fit, and the document paints them in one pass anyway.
        for (int64_t id : moving) changed = scripted_candidates_.erase(id) > 0 || changed;
    } else {
        for (int64_t id : moving) {
            auto [it, fresh] = scripted_hits_.try_emplace(id);
            ScriptedHit& hit = it->second;
            if (!fresh && hit.batch == ops_batch_) continue;
            bool continues = !fresh && (ops_batch_ - hit.batch <= SCRIPTED_STREAK_BATCHES || now - hit.at <= SCRIPTED_STREAK_GAP_S);
            hit = ScriptedHit{continues ? hit.count + 1 : 1, ops_batch_, now};
            if (hit.count >= 3 && scripted_candidates_.size() < MAX_LAYERS && scripted_candidates_.insert(id).second) changed = true;
        }
    }
    if (scripted_hits_.size() > 4096)
        for (auto it = scripted_hits_.begin(); it != scripted_hits_.end();)
            it = ops_batch_ - it->second.batch >= 8 && now - it->second.at >= 1 ? scripted_hits_.erase(it) : std::next(it);
    if (changed) composition_key_ = 0;
}

void Renderer::note_svg(const Json::Array& ops) {
    std::set<int64_t> touched;
    for (const Json& op : ops) {
        const std::string& kind = op["op"].asString();
        // An insertion changes its parent; a removed node is gone, and so is the way to its SVG.
        const Node* n = node(int64_t(kind == "ins" ? op["parent"].asNumber() : op["id"].asNumber()));
        int64_t root = 0;
        for (; n; n = n->parent)
            if (n->kind == Node::Element && n->tag == "svg") root = n->id;
        if (root) touched.insert(root);
    }
    std::unordered_map<int64_t, int> streaks;
    bool added = false;
    for (int64_t id : touched) {
        auto it = svg_streaks_.find(id);
        int count = streaks[id] = it == svg_streaks_.end() ? 1 : it->second + 1;
        if (count >= 3 && svg_layers_.size() < MAX_LAYERS && svg_layers_.insert(id).second) added = true;
    }
    svg_streaks_ = std::move(streaks);
    if (added) composition_key_ = 0;
    // A drawing whose shape is a layer of its own changed: a paint-only patch (cx, cy, r) lays nothing out, so the
    // composition would keep the layer's track about where the shape was.
    for (const auto& [id, p] : promoted_) {
        if (!p.svg_child) continue;
        for (const Node* n = node(id); n; n = n->parent)
            if (n->kind == Node::Element && n->tag == "svg" && touched.count(n->id)) composition_key_ = 0;
    }
}

bool Renderer::scripted_key(int64_t id, const Promotion& p, LayerKey& out) const {
    // A ::before or ::after has no style attribute: what its rules declare (p.base) is all.
    const Node* n = id < 0 ? nullptr : node(id);
    if (!n && !(id < 0 && node(pseudo_owner(id)))) return false;
    auto value = [&](const char* property) -> std::string {
        if (n)
            for (const auto& [name, v] : n->styles)
                if (name == property) return v;
        if (auto it = p.base.find(property); it != p.base.end()) return it->second;
        return animator_.base_value(id, property);
    };
    const Rect& box = p.box;
    auto origin = css::parse_origin(value("transform-origin"), box.w, box.h);
    auto translate = css::parse_translate(value("translate"), box.w, box.h);
    auto rotate = css::parse_rotate(value("rotate"));
    auto scale = css::parse_scale(value("scale"));
    auto transform = css::parse_transform(value("transform"), box.w, box.h);
    auto opacity = css::parse_opacity(value("opacity"));
    if (!origin || !translate || !rotate || !scale || !transform || !opacity) return false;
    Affine to = Affine::translate(box.x + origin->first, box.y + origin->second);
    Affine from = Affine::translate(-(box.x + origin->first), -(box.y + origin->second));
    out.t = 0;
    out.matrix = to * *translate * *rotate * *scale * *transform * from;
    out.opacity = *opacity;
    return true;
}

bool Renderer::still_track(int64_t id, Promotion& p, double now) {
#ifdef TW_LITEHTML_TWIDGET
    p.base = container_->declared(id, {"transform", "translate", "rotate", "scale", "opacity", "transform-origin"});
    LayerKey key;
    if (!scripted_key(id, p, key)) return false;
    auto old = promoted_.find(id);
    if (old != promoted_.end() && old->second.signature == p.signature && std::memcmp(&old->second.box, &p.box, sizeof(Rect)) == 0 &&
        old->second.track.keys.size() == 1 && old->second.track.keys[0].matrix == key.matrix && old->second.track.keys[0].opacity == key.opacity) {
        p.track = old->second.track;
        p.track_version = old->second.track_version;
    } else {
        p.track = LayerTrack{};
        p.track.start = now;
        p.track.keys.assign(1, key);
        p.track_version = next_version_++;
    }
    p.reach = bounds_under(p.box, key.matrix);
    if (p.has_clip) p.reach = intersection(p.reach, p.clip);
    return true;
#else
    (void)id;
    (void)p;
    (void)now;
    return false;
#endif
}

bool Renderer::apply_scripted(const std::vector<int64_t>& ids) {
    std::vector<std::pair<int64_t, LayerKey>> moves;
    for (int64_t id : ids) {
        const Promotion& p = promoted_.at(id);
        LayerKey key;
        if (!scripted_key(id, p, key)) return false;
        const Affine& m = key.matrix;
        if (std::sqrt(std::fabs(m.a * m.d - m.b * m.c)) > p.raster_scale + 0.001f) return false;
        // Where the layer shows must stay where the plan found nothing painting over it.
        Rect at = bounds_under(p.box, m);
        if (p.has_clip) at = intersection(at, p.clip);
        if (!at.empty() && !contains(p.reach, at)) return false;
        moves.emplace_back(id, key);
    }
    bool clean = !dirty_ || composition_only_;
    bool synced = true, changed = false;
    double now = now_s();
    for (const auto& [id, key] : moves) {
        Promotion& p = promoted_.at(id);
        if (p.track.keys.size() == 1 && p.track.keys[0].matrix == key.matrix && p.track.keys[0].opacity == key.opacity) continue;
        p.track.start = now;
        p.track.duration = 0;
        p.track.repeat = false;
        p.track.keys.assign(1, key);
        p.track_version = next_version_++;
        changed = true;
        auto layer = std::find_if(composition_.layers.begin(), composition_.layers.end(), [&](const Layer& l) { return l.id == id; });
        if (layer == composition_.layers.end()) {
            synced = false;
            continue;
        }
        layer->track = p.track;
        layer->track_version = p.track_version;
    }
    if (changed) {
        composition_.version = next_version_++;
        composition_only_ = clean && synced;
        dirty_ = true;
    }
    return true;
}

void Renderer::trim() {
    container_->trim();
    for (auto& [id, canvas] : canvases_) canvas->trim();
    // The paint summary goes too: the next paint is painted whole.
    painted_ = PaintSummary{};
    painted_valid_ = false;
    painted_list_ = DisplayList{};
    painted_list_valid_ = false;
    declared_generation_ = 0;
}

bool Renderer::damage(std::vector<Rect>& out) const {
    if (!damage_valid_) return false;
    out = damage_;
    return true;
}

std::string Renderer::damage_json() const {
    if (!damage_valid_) return "null";
    Json out = Json::array();
    for (const Rect& r : damage_) out.push(Json::array().push(double(r.x)).push(double(r.y)).push(double(r.w)).push(double(r.h)));
    return out.dump();
}

DisplayList Renderer::paint_document() {
    dirty_ = false;
#ifdef TW_LITEHTML_TWIDGET
    if (!promoted_.empty() || !composition_.layers.empty()) {
        std::vector<std::tuple<size_t, int, int64_t>> order;
        std::vector<litehtml::element::ptr> skip, backgrounds;
        for (const auto& [id, p] : promoted_) {
            if (auto el = container_->element(id)) {
                // A ring goes right under its element's layer.
                order.emplace_back(p.order, p.ring ? 0 : 1, id);
                if (!p.ring) (p.shimmer ? backgrounds : skip).push_back(std::move(el));
            }
        }
        DisplayList list = container_->draw(skip, backgrounds);
        std::sort(order.begin(), order.end());
        Composition next;
        bool changed = order.size() != composition_.layers.size();
        // A layer whose element and descendants nothing touched since the last paint keeps its content.
        bool all = touched_all_ || container_->layouts() != painted_layouts_;
        std::unordered_map<int64_t, const Layer*> before;
        if (!all)
            for (const Layer& old : composition_.layers) before.emplace(old.id, &old);
        for (const auto& [index, kind, id] : order) {
            const Promotion& p = promoted_.at(id);
            Layer layer;
            layer.id = id;
            auto old = before.find(id);
            auto redrawn = [&](const DisplayList& list) { return std::any_of(list.begin(), list.end(), [&](const Command& c) { return c.canvas != 0 && canvases_drawn_.count(c.canvas); }); };
            bool keep = old != before.end() && !redrawn(old->second->content) &&
                        std::none_of(touched_.begin(), touched_.end(), [&](int64_t t) { return t == id || is_ancestor(id, t); });
            if (p.shimmer || p.ring) {
                layer.content = p.tile;
                layer.bounds = p.box;
                layer.content_version = p.tile_version;
            } else if (keep) {
                // A selection change touches every layer: this one's tint is still the selection's.
                layer.content = old->second->content;
                layer.bounds = old->second->bounds;
                layer.content_version = old->second->content_version;
                paint_selection(layer.content, id, false);
            } else {
                layer.content = container_->draw_layer(container_->element(id), skip, layer.bounds);
                // Its ring layer under it is its shadow.
                if (p.shadowless)
                    layer.content.erase(std::remove_if(layer.content.begin(), layer.content.end(), [](const Command& c) { return c.kind == Cmd::Shadow; }),
                                        layer.content.end());
                paint_selection(layer.content, id, true);
                layer.content_version = content_version(layer.content);
            }
            layer.raster_scale = p.raster_scale;
            layer.has_clip = p.has_clip;
            layer.clip = p.clip;
            layer.clip_radii = p.clip_radii;
            layer.track_version = p.track_version;
            layer.track = p.track;
            layer.parent = p.parent;
            size_t i = next.layers.size();
            if (!changed) {
                const Layer& old = composition_.layers[i];
                changed = old.id != id || old.content_version != layer.content_version || old.track_version != layer.track_version || old.parent != layer.parent ||
                          std::memcmp(&old.bounds, &layer.bounds, sizeof(Rect)) != 0 || old.has_clip != layer.has_clip ||
                          std::memcmp(&old.clip, &layer.clip, sizeof(Rect)) != 0 || std::memcmp(&old.clip_radii, &layer.clip_radii, sizeof(Radii)) != 0;
            }
            next.layers.push_back(std::move(layer));
        }
        next.version = changed ? next_version_++ : composition_.version;
        composition_ = std::move(next);
        touched_.clear();
        touched_all_ = false;
        painted_layouts_ = container_->layouts();
        return list;
    }
#endif
    touched_.clear();
    touched_all_ = true;
    return container_->draw();
}

// ───────────────────────── queries ─────────────────────────

Json Renderer::answer(const Json& q) {
    const std::string& kind = q["q"].asString();
    litehtml::element::ptr el;
    const Node* shadow = nullptr;
    if (q.find("id")) {
        int64_t id = int64_t(q["id"].asNumber());
        shadow = node(id);
        el = container_->element(id);
        if (!shadow || shadow->kind != Node::Element) return Json::object().set("error", "no such element");
    }
    auto placement = [&]() -> Rect { return el ? script_box_of(el) : Rect{}; };
    // getBoundingClientRect is where the element shows, transforms included; the offset and client
    // sizes below are layout sizes, which transforms leave alone.
    if (kind == "rect") return rect_json(el ? visual_box_of(el) : Rect{});
    if (kind == "rects") {
        if (!el) return Json::array();
        // A wrapped inline element has a rect per line.
        Json rects = Json::array();
        for (const Rect& line : inline_boxes_of(el)) rects.push(rect_json(transformed_box(el, line)));
        return rects.arr().empty() ? Json::array().push(rect_json(visual_box_of(el))) : rects;
    }
    if (kind == "box") {
        Rect r = placement();
        float bl = 0, bt = 0, br = 0, bb = 0;
        auto ri = el ? el->get_render_item() : nullptr;
        if (ri) {
            const auto& bd = ri->get_borders();
            bl = float(bd.left); bt = float(bd.top); br = float(bd.right); bb = float(bd.bottom);
        }
        Json o = Json::object();
        double left = r.x, top = r.y;
        int64_t parent = 0;
#ifdef TW_LITEHTML_TWIDGET
        if (ri) offsets_of(ri, left, top, parent);
#endif
        o.set("offsetWidth", r.w).set("offsetHeight", r.h).set("offsetTop", top).set("offsetLeft", left).set("offsetParent", double(parent));
        float cw = std::max(0.0f, r.w - bl - br), ch = std::max(0.0f, r.h - bt - bb), sw = cw, sh = ch;
        if (ri) {
            // Only a scroll container counts its children's end margins and its own end padding.
            bool scroller = ri->css().get_overflow() != litehtml::overflow_visible;
            const auto& pad = ri->get_paddings();
            float right = -INFINITY, bottom = -INFINITY;
            content_extent(ri, right, bottom, scroller);
            if (right > -INFINITY) sw = std::max(sw, right + (scroller ? float(pad.right) : 0) - (r.x + bl));
            if (bottom > -INFINITY) sh = std::max(sh, bottom + (scroller ? float(pad.bottom) : 0) - (r.y + bt));
        }
#ifdef TW_LITEHTML_TWIDGET
        // A textarea's text can outgrow its rows: scrollHeight is the text's, clientHeight the box's.
        if (auto text = std::dynamic_pointer_cast<litehtml::render_item_inline_context>(ri); text && shadow && shadow->tag == "textarea") {
            const auto& pad = ri->get_paddings();
            auto content = text->text_content_size();
            sw = std::max(cw, float(content.width) + float(pad.left) + float(pad.right));
            sh = std::max(ch, float(content.height) + float(pad.top) + float(pad.bottom));
        }
#endif
        o.set("clientWidth", cw).set("clientHeight", ch).set("clientTop", bt).set("clientLeft", bl);
        o.set("scrollWidth", sw).set("scrollHeight", sh).set("scrollTop", 0).set("scrollLeft", 0);
        return o;
    }
    if (kind == "cs") {
        if (!el) return q.find("prop") ? Json("") : Json::object();
        // `prop` is only ever a custom property: the shim reads the rest from the whole answer.
        if (q.find("prop")) return computed_custom_property(el, q["prop"].asString());
        ComputedExtras extras;
        // From the animator rather than overrides_: a compositor layer's values never reach those.
        StyleValues animated;
#ifdef TW_LITEHTML_TWIDGET
        if (shadow && !still()) animated = animator_.peek(shadow->id, now_s());
#endif
        extras.animated = &animated;
        return computed_style(el, q["pseudo"].asString(), extras);
    }
    if (kind == "elementFromPoint") return double(widget_id_at(q["x"].asNumber(), q["y"].asNumber(), nullptr));
    if (kind == "measureText") return measure_text(q["font"].isString() ? q["font"].asString() : "10px sans-serif", q["text"].asString());
    if (kind == "validity") return Json::object().set("valid", true).set("message", "");
    if (kind == "matchMedia") return Json::object().set("matches", match_media(q["query"].asString()));
    if (kind == "viewport") return Json::object().set("innerWidth", width_).set("innerHeight", double(vp_height_)).set("dpr", dpr_);
    if (kind == "anims") {
        std::vector<int64_t> ids;
        if (shadow) {
            std::function<void(const Node*)> visit = [&](const Node* n) {
                if (n->id && ids.size() < 100000) ids.push_back(n->id);
                if (q["subtree"].asBool())
                    for (const Node* c : n->children) visit(c);
            };
            visit(shadow);
        }
        return Json::object().set("list", animator_.css_animations(ids, now_s()));
    }
    if (kind == "imageData") {
        int w = std::clamp(q["w"].asInt(), 0, Canvas::MAX_SIDE), h = std::clamp(q["h"].asInt(), 0, Canvas::MAX_SIDE);
        if (int64_t(w) * h > Canvas::MAX_PIXELS) return Json::object().set("error", "too large");
        std::vector<uint8_t> rgba;
        auto it = shadow ? canvases_.find(shadow->id) : canvases_.end();
        if (it == canvases_.end() || !backend_.canvas_read(it->second->id(), q["x"].asInt(), q["y"].asInt(), w, h, rgba) || rgba.size() != size_t(w) * h * 4)
            rgba.assign(size_t(w) * h * 4, 0);
        binary_ = base64(rgba);
        return Json::object().set("width", w).set("height", h);
    }
    if (kind == "canvasCall") {
        auto it = shadow ? canvases_.find(shadow->id) : canvases_.end();
        const std::string& method = q["method"].asString();
        if (it == canvases_.end()) {
            if (method == "getTransform") return Json::array().push(1).push(0).push(0).push(1).push(0).push(0);
            if (method == "getLineDash") return Json::array();
            if (method == "isPointInPath" || method == "isPointInStroke") return false;
            return Json::object().set("error", "no canvas");
        }
        if (method == "toDataURL") return Json::object().set("error", "toDataURL unsupported by the litehtml renderer");
        return it->second->call(method, q["args"]);
    }
#ifdef TW_LITEHTML_TWIDGET
    if (kind == "svg") {
        const std::string& method = q["method"].asString();
        litehtml::position text;
        if (el && litehtml::svg_text_box(el, container_.get(), text)) {
            if (method == "getComputedTextLength") return double(text.width);
            if (method == "getBBox") return Json::object().set("x", double(text.x)).set("y", double(text.y)).set("width", double(text.width)).set("height", double(text.height));
        }
        litehtml::vector_path path;
        if (!el || !litehtml::svg_geometry(el, path)) return Json::object().set("error", "unsupported");
        if (method == "getTotalLength") return double(litehtml::svg_path_length(path, -1, nullptr, nullptr));
        if (method == "getPointAtLength") {
            double at = q["args"][size_t(0)].asNumber();
            float x = 0, y = 0;
            litehtml::svg_path_length(path, std::isfinite(at) ? float(std::max(0.0, at)) : 0.0f, &x, &y);
            return Json::object().set("x", x).set("y", y);
        }
        if (method == "getBBox") {
            float x0, y0, x1, y1;
            if (!path_bounds(path, x0, y0, x1, y1)) return Json::object().set("error", "unsupported");
            return Json::object().set("x", x0).set("y", y0).set("width", x1 - x0).set("height", y1 - y0);
        }
        return Json::object().set("error", "unsupported");
    }
#else
    if (kind == "svg") return Json::object().set("error", kind + " unsupported by the litehtml renderer");
#endif
    return Json::object().set("error", "unknown query");
}

bool Renderer::match_media(const std::string& query) const {
    std::string q = lower(query);
    if (q.find("prefers-color-scheme") != std::string::npos) return (q.find("dark") != std::string::npos) == dark_;
    auto px = [&](const char* key) -> double {
        size_t i = q.find(key);
        if (i == std::string::npos) return NAN;
        size_t j = i + std::strlen(key);
        while (j < q.size() && (q[j] == ' ' || q[j] == ':')) j++;
        return std::atof(q.c_str() + j);
    };
    double v;
    if (!std::isnan(v = px("max-width"))) return width_ <= v;
    if (!std::isnan(v = px("min-width"))) return width_ >= v;
    if (q.find("hover") != std::string::npos) return q.find("none") != std::string::npos;
    if (q.find("pointer") != std::string::npos) return q.find("coarse") != std::string::npos;
    return false;
}

int64_t Renderer::widget_id_at(double x, double y, Rect* placement) const {
#ifdef TW_LITEHTML_TWIDGET
    // A layer shows where its track has it now, not where layout put it: the topmost layer under the
    // point takes it, mapped back into the document.
    if (!promoted_.empty()) {
        std::vector<std::pair<size_t, int64_t>> order;
        for (const auto& [id, p] : promoted_) order.emplace_back(p.order, id);
        std::sort(order.rbegin(), order.rend());
        double now = now_s();
        for (const auto& [rank, id] : order) {
            const Promotion& p = promoted_.at(id);
            // A shimmer's element stays in the document, which takes the point; a ring is its element's shadow.
            if (p.shimmer || p.ring) continue;
            if (p.has_clip && !(x >= p.clip.x && y >= p.clip.y && x < p.clip.x + p.clip.w && y < p.clip.y + p.clip.h)) continue;
            Affine shown, inverse;
            float opacity = 1;
            if (!layer_shown(id, now, shown, opacity) || opacity <= 0 || !shown.invert(inverse)) continue;
            float mx = inverse.apply_x(float(x), float(y)), my = inverse.apply_y(float(x), float(y));
            if (!(mx >= p.box.x && my >= p.box.y && mx < p.box.x + p.box.w && my < p.box.y + p.box.h)) continue;
            if (int64_t hit = container_->widget_id(container_->element_at_within(container_->element(id), mx, my), placement)) return hit;
        }
    }
#endif
    return container_->widget_id(container_->element_at(x, y), placement);
}

bool Renderer::layer_shown(int64_t id, double now, Affine& shown, float& opacity) const {
    shown = {};
    opacity = 1;
#ifdef TW_LITEHTML_TWIDGET
    // Its key after each parent's, the opacity theirs too.
    auto it = promoted_.find(id);
    for (const Promotion* q = it == promoted_.end() ? nullptr : &it->second; q; q = q->parent ? &promoted_.at(q->parent) : nullptr) {
        double span = 0;
        auto keys = resume_track(q->track, now - q->track.start, span);
        if (keys.empty() || promoted_.count(q->parent) != size_t(q->parent != 0)) return false;
        shown = keys.front().matrix * shown;
        opacity *= keys.front().opacity;
    }
    return it != promoted_.end();
#else
    (void)id;
    (void)now;
    return false;
#endif
}

// ───────────────────────── events ─────────────────────────

twidget::WidgetGestureClaim Renderer::gesture_claim(int64_t target) const {
    bool prevented = prevent_.count(std::to_string(target) + ":pointerdown") || prevent_.count(std::to_string(target) + ":touchstart");
    std::vector<std::string> values;
    // Up to the widget's <html> itself: touch-action on an ancestor covers its descendants, as in CSS.
    for (const Node* n = target ? node(target) : html_node_.get(); n; n = n->parent) {
        // From any stylesheet: the fork keeps the declaration as written, which is all this needs.
        if (auto el = n->id ? container_->element(n->id) : nullptr) values.push_back(computed_touch_action(el));
    }
    return twidget::gesture::claimOf(values, prevented);
}

std::optional<double> Renderer::hold_deadline() const {
    return arbiter_pointer_ >= 0 && !frozen_ ? arbiter_.holdDeadline() : std::nullopt;
}

void Renderer::check_hold(std::vector<Json>& out) {
    auto at = hold_deadline();
    if (!at || now_ms() < *at) return;
    if (auto v = arbiter_.tick(now_ms())) settle_gesture(*v, arbiter_pointer_, out);
}

void Renderer::settle_gesture(const twidget::GestureVerdict& v, int pointer, std::vector<Json>& out) {
    auto it = touches_.find(pointer);
    if (it == touches_.end()) return;
    Touch& t = it->second;
    if (v.event == twidget::GestureEvent::ContextMenu) {
        t.held = v.gesture == twidget::Gesture::Hold;
        int64_t target = t.capture ? t.capture : t.target;
        if (target) emit_event("contextmenu", target, t.capture ? box_of(target) : t.target_rect, t, 0, out);
    }
    if (v.cancel && bubble_) bubble_takes(pointer, v, out);
}

void Renderer::bubble_takes(int pointer, const twidget::GestureVerdict& v, std::vector<Json>& out) {
    auto it = touches_.find(pointer);
    if (it == touches_.end()) return;
    Touch done = it->second;
    // Nothing more of this pointer reaches the widget: its later moves and its release find no touch.
    touches_.erase(it);
    if (pointer == arbiter_pointer_) {
        arbiter_.cancel();
        arbiter_pointer_ = -1;
    }
    if (selection_.pointer == pointer) selection_.pointer = -1;
    if (done.button == 0 && container_->mouse(3, done.x, done.y)) restyled_by_input();
    if (done.target) {
        emit_event("pointercancel", done.capture ? done.capture : done.target, done.capture ? box_of(done.capture) : done.target_rect, done, 3, out);
        emit_event("touchcancel", done.target, done.target_rect, done, 3, out);
    }
    set_capture(done, 0, out);
    out.push_back(Json::object()
                      .set("type", "bubble")
                      .set("action", std::string(twidget::gesture::name(v.action.value_or(twidget::BubbleAction::Menu))))
                      .set("gesture", std::string(twidget::gesture::name(v.gesture)))
                      .set("x", done.x)
                      .set("y", done.y)
                      .set("edge", v.edge));
}

Json Renderer::event_json(const char* type, int64_t target, const Rect& target_rect, const Input& in) const {
    Json ev = Json::object();
    ev.set("type", type).set("targetId", double(target));
    ev.set("clientX", in.x).set("clientY", in.y).set("pageX", in.x).set("pageY", in.y);
    ev.set("offsetX", in.x - target_rect.x).set("offsetY", in.y - target_rect.y);
    ev.set("movementX", in.movement_x).set("movementY", in.movement_y);
    ev.set("button", in.button).set("buttons", in.buttons).set("detail", in.detail).set("modifiers", in.modifiers);
    if (in.related) ev.set("relatedId", double(in.related));
    std::string name = type;
    if (name.rfind("pointer", 0) == 0 || name.find("pointercapture") != std::string::npos || name == "click" || name == "dblclick" || name == "contextmenu") {
        ev.set("pointerId", double(in.mouse ? 1 : in.pointer + 2)).set("pointerType", in.mouse ? "mouse" : "touch").set("isPrimary", in.primary).set("pressure", in.buttons ? 0.5 : 0.0);
    }
    // The sandbox runs a click's default action (a link, a label, a checkbox) unless a listener prevents it.
    if (name == "click") ev.set("defaultActions", true);
    // Form control state travels with every event (the sandbox mirror relies on it).
    const Node* n = node(target);
    if (n && (n->tag == "input" || n->tag == "textarea" || n->tag == "select" || n->tag == "button")) {
        bool checkbox = false;
        for (const auto& a : n->attrs) if (a.first == "type") checkbox = lower(a.second) == "checkbox" || lower(a.second) == "radio";
        if (checkbox) {
            bool checked = false;
            for (const auto& a : n->attrs) if (a.first == "checked") checked = true;
            ev.set("checked", checked);
        }
        if (n->tag == "textarea") ev.set("value", n->data);
        else for (const auto& a : n->attrs) if (a.first == "value") ev.set("value", a.second);
    }
    return ev;
}

Json Renderer::event_message(Json ev, bool trusted) {
    // Every event comes from real input (no script runs in this document): trusted, like the WebView
    // renderer's isTrusted, which the hosts require before they count a user gesture; a finger's press is not.
    return Json::object().set("type", "event").set("ev", std::move(ev)).set("trusted", trusted);
}

void Renderer::dispatch(Json ev, std::vector<Json>& out, bool trusted) {
    if (frozen_) return;
    const std::string& type = ev["type"].asString();
    if (!listened(type.c_str())) return;
    if (coalesced(type)) {
        std::string key = type + ":" + std::to_string(int64_t(ev["pointerId"].asNumber()));
        for (auto& [k, pending] : pending_moves_) {
            if (k != key) continue;
            // Wheel deltas add up over the frame; a move only keeps its latest position.
            if (type == "wheel") {
                ev.set("deltaX", ev["deltaX"].asNumber() + pending["deltaX"].asNumber());
                ev.set("deltaY", ev["deltaY"].asNumber() + pending["deltaY"].asNumber());
            } else {
                ev.set("movementX", ev["movementX"].asNumber() + pending["movementX"].asNumber());
                ev.set("movementY", ev["movementY"].asNumber() + pending["movementY"].asNumber());
            }
            pending = std::move(ev);
            return;
        }
        pending_moves_.emplace_back(std::move(key), std::move(ev));
        return;
    }
    // Anything else keeps its order relative to the moves before it.
    flush_moves(out);
    out.push_back(event_message(std::move(ev), trusted));
}

void Renderer::flush_moves(std::vector<Json>& out) {
    for (auto& [key, ev] : pending_moves_) out.push_back(event_message(std::move(ev)));
    pending_moves_.clear();
}

int64_t Renderer::text_control(int64_t id) const {
    const Node* n = node(id);
    for (int depth = 0; n && depth < 6; n = n->parent, depth++) {
        if (n->kind != Node::Element || n->svg || (n->tag != "input" && n->tag != "textarea")) continue;
        std::string type;
        for (const auto& [name, value] : n->attrs) {
            if (name == "disabled") return 0;
            if (name == "type") type = css::trim(lower(value));
        }
        bool editable = n->tag == "textarea" || type.empty() || type == "text" || type == "search" || type == "number" || type == "email" || type == "url" || type == "tel";
        return editable && attached(n) ? n->id : 0;
    }
    return 0;
}

std::string Renderer::control_value(const Node* n) const {
    if (n->tag == "textarea") return n->data;
    for (const auto& [name, value] : n->attrs)
        if (name == "value") return value;
    return {};
}

void Renderer::form_event(const char* type, int64_t id, std::vector<Json>& out, const char* input_type) {
    if (frozen_ || !listened(type)) return;
    Json ev = Json::object();
    ev.set("type", type).set("targetId", double(id));
    if (const Node* n = node(id)) ev.set("value", control_value(n));
    if (input_type) ev.set("inputType", input_type);
    dispatch(std::move(ev), out);
}

void Renderer::focus(int64_t id, std::vector<Json>& out) {
    if (frozen_) return;
    if (editing_.id == id) {
        // Focused again: the platform shows its editor again.
        editing_.generation = ++editor_generation_;
        return;
    }
    if (editing_.id) blur(out);
    Node* n = node(id);
    if (!n) return;
    editing_ = EditorSpec{};
    editing_.id = id;
    editing_.generation = ++editor_generation_;
    editing_.multiline = n->tag == "textarea";
    editing_.text = control_value(n);
    editing_initial_ = editing_.text;
    for (const auto& [name, value] : n->attrs) {
        if (name == "placeholder") editing_.placeholder = value;
        else if (name == "maxlength") editing_.max_length = std::max(-1, std::atoi(value.c_str()));
        else if (name == "readonly") editing_.read_only = true;
        else if (name == "type" && !editing_.multiline) editing_.input_type = css::trim(lower(value));
    }
    if (!editing_.multiline && editing_.input_type.empty()) editing_.input_type = "text";
#ifdef TW_LITEHTML_TWIDGET
    container_->editor_style(id, editing_);
#endif
    // The control's text goes transparent in the document: its style attribute changes.
    // Its placeholder comes and goes too, which only a build takes.
    patch_log_.valid = false;
    note_attribute(n, "style");
    document_changed_ = true;
    stale_ = true;
    form_event("focus", id, out);
    form_event("focusin", id, out);
}

void Renderer::blur(std::vector<Json>& out) {
    int64_t id = editing_.id;
    if (!id) return;
    editing_ = EditorSpec{};
    editing_.generation = ++editor_generation_;
    Node* n = node(id);
    if (!n) return;
    // Its placeholder comes and goes too, which only a build takes.
    patch_log_.valid = false;
    note_attribute(n, "style");
    document_changed_ = true;
    stale_ = true;
    if (control_value(n) != editing_initial_) form_event("change", id, out);
    form_event("blur", id, out);
    form_event("focusout", id, out);
}

void Renderer::refresh_editor() {
    if (!editing_.id) return;
    const Node* n = node(editing_.id);
    if (!n || !attached(n)) {
        editing_ = EditorSpec{};
        editing_.generation = ++editor_generation_;
        return;
    }
#ifdef TW_LITEHTML_TWIDGET
    EditorSpec before = editing_;
    container_->editor_style(editing_.id, editing_);
    // The document draws the edited text transparent: the editor keeps the colour it had.
    editing_.color = before.color;
    bool moved = std::memcmp(&before.frame, &editing_.frame, sizeof(Rect)) != 0 || before.has_clip != editing_.has_clip ||
                 std::memcmp(&before.clip, &editing_.clip, sizeof(Rect)) != 0 || before.font != editing_.font || before.font_size != editing_.font_size ||
                 before.align != editing_.align || std::memcmp(&before.color, &editing_.color, sizeof(Color)) != 0;
    if (moved) editing_.generation = ++editor_generation_;
#endif
}

std::string Renderer::editor_json() const {
    if (!editing_.id) return "null";
    const EditorSpec& e = editing_;
    auto rect = [](const Rect& r) { return Json::array().push(double(r.x)).push(double(r.y)).push(double(r.w)).push(double(r.h)); };
    Json j = Json::object();
    j.set("id", double(e.id)).set("generation", double(e.generation)).set("multiline", e.multiline).set("frame", rect(e.frame));
    if (e.has_clip) j.set("clip", rect(e.clip));
    j.set("font", e.font).set("fontFamily", e.font_family).set("fontSize", double(e.font_size)).set("fontWeight", e.font_weight).set("italic", e.italic);
    j.set("color", double((uint32_t(e.color.a) << 24) | (uint32_t(e.color.r) << 16) | (uint32_t(e.color.g) << 8) | e.color.b));
    j.set("align", e.align).set("text", e.text).set("placeholder", e.placeholder).set("inputType", e.input_type);
    j.set("maxLength", e.max_length).set("readOnly", e.read_only).set("selectionStart", e.selection_start).set("selectionEnd", e.selection_end);
    return j.dump();
}

std::vector<Json> Renderer::edit(uint64_t generation, const std::string& text, const std::string& input_type) {
    std::vector<Json> out;
    last_input_ = now_s();
    if (frozen_ || !editing_.id || generation != editing_.generation) return out;
    Node* n = node(editing_.id);
    if (!n) return out;
    editing_.text = text;
    editing_.selection_start = editing_.selection_end = -1;
    // The value goes in as a script setting it would; the text stays hidden under the editor.
    Json op = Json::object();
    op.set("op", "prop").set("id", double(n->id)).set("name", "value").set("value", text);
    document_changed_ = false;
    apply_op(op, out);
    editing_.generation = generation;
    if (document_changed_) {
        rebuild(true);
        report_size(out);
    }
    form_event("input", n->id, out, input_type.c_str());
    flush_pending(out);
    return out;
}

std::vector<Json> Renderer::end_editing() {
    std::vector<Json> out;
    blur(out);
    if (stale_) {
        rebuild(true);
        report_size(out);
    }
    flush_pending(out);
    return out;
}

bool Renderer::listened(const char* type) const {
    if (std::none_of(std::begin(PROFILE_EVENTS), std::end(PROFILE_EVENTS), [&](const char* e) { return std::strcmp(type, e) == 0; })) return false;
    for (const char* a : ALWAYS_FORWARD)
        if (std::strcmp(type, a) == 0) return true;
    return interest_.count(type) != 0;
}

void Renderer::send_event(const char* type, int64_t target, const Input& in, std::vector<Json>& out) {
    if (listened(type)) dispatch(event_json(type, target, box_of(target), in), out);
}

void Renderer::emit_event(const char* type, int64_t target, const Rect& target_rect, const Touch& t, int action, std::vector<Json>& out) {
    if (t.mouse && std::strncmp(type, "touch", 5) == 0) return;
    // An event nothing listens for is dropped before it is built.
    if (!listened(type)) return;
    Input in;
    in.x = t.x;
    in.y = t.y;
    in.pointer = t.pointer;
    in.primary = t.primary;
    in.mouse = t.mouse;
    in.modifiers = t.modifiers;
    in.button = action == 1 ? 0 : t.button;
    in.buttons = action == 0 || action == 1 ? (t.button == 2 ? 2 : 1) : 0;
    in.detail = std::strcmp(type, "click") == 0 || std::strcmp(type, "dblclick") == 0 ? click_count_ : 0;
    if (action == 1) {
        in.movement_x = t.x - t.sent_x;
        in.movement_y = t.y - t.sent_y;
    }
    // A finger's or a pen's press may still become the chat's scroll, which ends it in pointercancel: its press is no
    // gesture, its release is (widget-sandbox v0.3.0, a finger's press counts from its release).
    bool trusted = !(t.finger && (std::strcmp(type, "pointerdown") == 0 || std::strcmp(type, "mousedown") == 0));
    dispatch(event_json(type, target, target_rect, in), out, trusted);
}

Rect Renderer::box_of(int64_t id) const {
    auto el = container_->element(id);
    return el ? border_box_of(el) : Rect{};
}

void Renderer::set_capture(Touch& t, int64_t id, std::vector<Json>& out) {
    if (t.capture == id) return;
    int64_t previous = t.capture;
    t.capture = id;
    if (previous) emit_event("lostpointercapture", previous, box_of(previous), t, 2, out);
    if (id) emit_event("gotpointercapture", id, box_of(id), t, 1, out);
}

const Renderer::Node* Renderer::parent_node(int64_t id) const {
    if (id < 0) return node(pseudo_owner(id));
    const Node* n = node(id);
    return n ? n->parent : nullptr;
}

bool Renderer::is_ancestor(int64_t ancestor, int64_t id) const {
    if (!ancestor) return false;
    for (const Node* p = parent_node(id); p; p = p->parent)
        if (p->id == ancestor) return true;
    return false;
}

std::vector<int64_t> Renderer::chain(int64_t id) const {
    std::vector<int64_t> out;
    for (const Node* n = node(id); n; n = n->parent)
        if (n->id) out.push_back(n->id);
    return out;
}

std::vector<Json> Renderer::touch(int action, double x, double y, int pointer, bool primary, bool mouse, int modifiers, int button) {
    std::vector<Json> out;
    last_input_ = now_s();
    settle(out);
    if (frozen_) return out;
    if (action == 0) {
        // Playing a phone's bubble, a mouse's main button is a finger, for the widget too.
        bool finger = !mouse || (bubble_touch_ && button != 2);
        Touch t{x, y, x, y, 0, Rect{}, false, primary, pointer, !finger, modifiers};
        t.button = button;
        t.finger = !mouse;
        t.target = widget_id_at(x, y, &t.target_rect);
        touches_[pointer] = t;
        bool selecting = false;
        if (primary) {
            press_id_ = t.target;
            press_y_ = y;
            press_at_ = now_s();
            // A press drops the selection; a mouse's on selectable text starts the next one there.
            bool had = !(selection_.anchor == selection_.focus);
            selection_ = {};
            TextPoint at;
            if (t.mouse && button == 0 && t.target && starts_selection(t.target) && point_at(x, y, text_runs(), at)) {
                selection_.anchor = selection_.focus = at;
                selection_.pointer = pointer;
                selecting = true;
            }
            if (had) selection_changed();
        }
        std::optional<twidget::GestureVerdict> verdict;
        if (primary) {
            // A drag that selects claims what a claim can: horizontal drags (a mouse never pans the chat anyway).
            twidget::WidgetGestureClaim claim = selecting ? twidget::WidgetGestureClaim{true, false, false} : gesture_claim(t.target);
            out.push_back(Json::object().set("type", "gesture").set("wants", claim.y).set("x", claim.x).set("y", claim.y).set("hold", claim.hold));
            arbiter_pointer_ = pointer;
            verdict = arbiter_.down(now_ms(), arbiter_x(x), y, claim, t.mouse ? twidget::PointerKind::Mouse : twidget::PointerKind::Touch, button);
        }
        if (button == 0 && container_->mouse(0, x, y)) restyled_by_input();
        if (t.target) {
            emit_event("pointerdown", t.target, t.target_rect, t, 0, out);
            emit_event("mousedown", t.target, t.target_rect, t, 0, out);
            emit_event("touchstart", t.target, t.target_rect, t, 0, out);
        }
        if (verdict) settle_gesture(*verdict, pointer, out);
        flush_pending(out);
        return out;
    }
    auto it = touches_.find(pointer);
    if (it == touches_.end()) return out;
    Touch& t = it->second;
    t.x = x;
    t.y = y;
    t.modifiers = modifiers;
    std::optional<twidget::GestureVerdict> verdict;
    if (pointer == arbiter_pointer_) {
        if (action == 1) verdict = arbiter_.move(now_ms(), arbiter_x(x), y);
        else {
            if (action == 2) verdict = arbiter_.up(now_ms(), arbiter_x(x), y);
            else arbiter_.cancel();
            arbiter_pointer_ = -1;
        }
    }
    if (action == 1 && selection_.pointer == pointer) {
        TextPoint at;
        if (point_at(x, y, text_runs(), at) && !(at == selection_.focus)) {
            selection_.focus = at;
            selection_changed();
        }
    }
    if (action == 1) {
        // A finger jitters a few px during a tap: moves inside the policy's slop are not forwarded.
        if (!t.moved && std::hypot(x - t.x0, y - t.y0) <= twidget::gesture::TOUCH_SLOP_PX) {
            if (verdict) settle_gesture(*verdict, pointer, out);
            return out;
        }
        t.moved = true;
        if (container_->mouse(1, x, y)) restyled_by_input();
        if (t.target) {
            // A finger's events stay with the element it pressed; a mouse's go to the element under
            // it. Either way a capture takes the pointer events.
            Rect rect = t.target_rect;
            int64_t under = t.mouse ? widget_id_at(x, y, &rect) : t.target;
            if (t.capture && !container_->element(t.capture)) set_capture(t, 0, out);
            if (t.capture) emit_event("pointermove", t.capture, box_of(t.capture), t, 1, out);
            else if (under) emit_event("pointermove", under, rect, t, 1, out);
            if (under) emit_event("mousemove", under, rect, t, 1, out);
            emit_event("touchmove", t.target, t.target_rect, t, 1, out);
        }
        t.sent_x = x;
        t.sent_y = y;
        if (verdict) settle_gesture(*verdict, pointer, out);
        return out;
    }
    // Playing the bubble, a press it takes at the release (a drag past the slop there) ends with pointercancel.
    if (verdict && verdict->cancel && bubble_) {
        bubble_takes(pointer, *verdict, out);
        flush_pending(out);
        return out;
    }
    Touch done = t;
    touches_.erase(it);
    if (selection_.pointer == pointer) selection_.pointer = -1;
    if (done.button == 0 && container_->mouse(action, x, y, done.mouse && action == 2)) restyled_by_input();
    if (action == 2) {
        Rect up_rect;
        int64_t up_target = widget_id_at(x, y, &up_rect);
        if (done.target) {
            int64_t under = done.mouse ? up_target : done.target;
            const Rect& under_rect = done.mouse ? up_rect : done.target_rect;
            if (done.capture && !container_->element(done.capture)) set_capture(done, 0, out);
            if (done.capture) emit_event("pointerup", done.capture, box_of(done.capture), done, 2, out);
            else if (under) emit_event("pointerup", under, under_rect, done, 2, out);
            if (under) emit_event("mouseup", under, under_rect, done, 2, out);
            emit_event("touchend", done.target, done.target_rect, done, 2, out);
        }
        // A click: the press and the release land on the same element, or one inside the other
        // (the click goes to the outer one). A finger must also have stayed inside the slop.
        int64_t click = 0;
        if (done.target && (done.mouse || !done.moved)) {
            if (up_target == done.target || is_ancestor(done.target, up_target)) click = done.target;
            else if (is_ancestor(up_target, done.target)) click = up_target;
        }
        // A secondary button never clicks, nor does a hold the widget took (it had its contextmenu).
        if (done.primary && click && done.button == 0 && !done.held) {
            bool twice = verdict && verdict->event == twidget::GestureEvent::DblClick;
            click_count_ = twice ? 2 : 1;
            Rect rect = click == done.target ? done.target_rect : up_rect;
            emit_event("click", click, rect, done, 2, out);
            if (twice) emit_event("dblclick", click, rect, done, 2, out);
            toggle_details(click, out);
            // A tap on a text control edits it; one anywhere else ends the editing.
            if (int64_t control = text_control(click)) focus(control, out);
            else if (editing_.id) blur(out);
            if (stale_) {
                rebuild(true);
                report_size(out);
            }
        }
    } else if (done.target) {
        emit_event("pointercancel", done.capture ? done.capture : done.target, done.capture ? box_of(done.capture) : done.target_rect, done, 3, out);
        emit_event("touchcancel", done.target, done.target_rect, done, 3, out);
    }
    set_capture(done, 0, out);
    flush_pending(out);
    return out;
}

std::vector<Json> Renderer::hover(int action, double x, double y, int modifiers) {
    std::vector<Json> out;
    last_input_ = now_s();
    settle(out);
    if (frozen_) return out;
    auto box = [this](int64_t id) { return box_of(id); };
    Input in;
    in.mouse = true;
    in.pointer = 1;
    in.modifiers = modifiers;
    int64_t previous = hover_;
    int64_t target = 0;
    if (action == 0) {
        in.movement_x = x - hover_x_;
        in.movement_y = y - hover_y_;
        in.x = hover_x_ = x;
        in.y = hover_y_ = y;
        if (container_->mouse(1, x, y)) restyled_by_input();
        target = widget_id_at(x, y, nullptr);
    } else {
        in.x = hover_x_;
        in.y = hover_y_;
        if (container_->leave()) restyled_by_input();
    }
    if (target != previous) {
        hover_ = target;
        // out/over bubble and name the other element; leave/enter go to each ancestor the pointer
        // actually left or entered, innermost first for leave and outermost first for enter.
        auto before = chain(previous), after = chain(target);
        auto in_chain = [](const std::vector<int64_t>& c, int64_t id) { return std::find(c.begin(), c.end(), id) != c.end(); };
        if (previous) {
            in.related = target;
            send_event("pointerout", previous, in, out);
            send_event("mouseout", previous, in, out);
            for (int64_t id : before) {
                if (in_chain(after, id)) continue;
                send_event("pointerleave", id, in, out);
                send_event("mouseleave", id, in, out);
            }
        }
        if (target) {
            in.related = previous;
            send_event("pointerover", target, in, out);
            send_event("mouseover", target, in, out);
            for (auto it = after.rbegin(); it != after.rend(); ++it) {
                if (in_chain(before, *it)) continue;
                send_event("pointerenter", *it, in, out);
                send_event("mouseenter", *it, in, out);
            }
        }
        in.related = 0;
    }
    if (target) {
        send_event("pointermove", target, in, out);
        send_event("mousemove", target, in, out);
    }
    flush_pending(out);
    return out;
}

// Profile 1 gives a widget no wheel and no keyboard (widget-sandbox docs/profile-v1.md, "Input"): both
// stay entry points so that hosts need not change, and do nothing.
std::vector<Json> Renderer::wheel(double, double, double, double, int, int) {
    return {};
}

std::vector<Json> Renderer::key(const std::string&, const std::string&, const std::string&, bool, int) {
    return {};
}

void Renderer::load_images(std::vector<Json>& out) {
    for (int64_t id : pending_images_) {
        const Node* n = node(id);
        if (!n || n->tag != "img") continue;
        const std::string* src = nullptr;
        for (const auto& [name, value] : n->attrs)
            if (name == "src") src = &value;
        if (!src || src->empty()) continue;
        // Loaded like the document will load it: a detached <img> (new Image()) loads too, as in a browser.
        int image = 0;
        float w = 0, h = 0;
        if (!container_->image_of(*src, image, w, h)) image_event("error", id, 0, 0, out);
        else if (w > 0 || h > 0) image_event("load", id, w, h, out);
        else waiting_images_[image].push_back(id);
    }
    pending_images_.clear();
}

void Renderer::image_event(const char* type, int64_t id, float w, float h, std::vector<Json>& out) {
    Json ev = Json::object();
    ev.set("type", type).set("targetId", double(id));
    if (std::strcmp(type, "load") == 0) ev.set("naturalWidth", w).set("naturalHeight", h);
    dispatch(std::move(ev), out);
}

std::vector<Json> Renderer::image_ready(int id, float w, float h) {
    std::vector<Json> out;
    if (auto it = waiting_images_.find(id); it != waiting_images_.end()) {
        for (int64_t node_id : it->second) image_event(w > 0 || h > 0 ? "load" : "error", node_id, w, h, out);
        waiting_images_.erase(it);
    }
    if (!container_->image_ready(id, w, h)) return out;
    rebuild();
    report_size(out);
    return out;
}

// ───────────────────────── frozen ─────────────────────────

void Renderer::freeze(const std::string& reason, bool fatal) {
    if (frozen_) return;
    frozen_ = true;
    frozen_reason_ = reason;
    frozen_fatal_ = fatal;
    touches_.clear();
    pending_moves_.clear();
    hover_ = 0;
}

// ───────────────────────── heavy boxes ─────────────────────────

namespace {

/// What a box adds (paint_policy.h, rules 1 to 4), from litehtml's computed values; `opacity` the element's when a
/// scripted layer holds a newer one than the document.
paint::BoxPaint box_paint_of(const litehtml::element& el, bool pseudo, std::optional<float> opacity) {
    const auto& css = el.css();
    paint::BoxPaint out;
    out.backdrop = !css.get_backdrop_filter().empty();
    bool blurred = false;
    for (const auto& shadow : css.get_box_shadows()) {
        if (!(shadow.blur.val() <= 0)) {
            blurred = true;
            break;
        }
    }
    // A disabled or pressed <button>'s opacity is its own dimming, the UA layer's or the author's.
    bool dimmed = !pseudo && el.tag() == litehtml::_button_ && (el.get_attr("disabled") || el.has_pseudo_class(litehtml::_active_));
    out.heavy = out.backdrop || !css.get_filter().empty() || blurred || (!dimmed && opacity.value_or(css.get_opacity()) < 1);
    return out;
}

} // namespace

std::optional<float> Renderer::layer_opacity(int64_t id) const {
    auto p = promoted_.find(id);
    if (p == promoted_.end() || !p->second.scripted || p->second.track.keys.size() != 1) return std::nullopt;
    return p->second.track.keys[0].opacity;
}

void Renderer::scan_boxes() {
#ifdef TW_LITEHTML_TWIDGET
    litehtml::document* doc = container_->doc();
    paint_.begin_scan();
    // The widget's elements carry their id; the renderer's own (the stopped badge) build no box of the widget's.
    auto visit = [&](auto& self, const litehtml::element::ptr& el, int64_t parent) -> void {
        if (el->is_text() || el->is_comment()) return;
        litehtml::string_id tag = el->tag();
        if (tag == litehtml::__tag_before_ || tag == litehtml::__tag_after_) {
            if (parent && el->css().get_display() != litehtml::display_none)
                paint_.set_box(parent * 4 + (tag == litehtml::__tag_before_ ? 1 : 2), box_paint_of(*el, true, std::nullopt));
            return;
        }
        if (el->css().get_display() == litehtml::display_none) return;
        const char* twid = el->get_attr("data-twid");
        int64_t id = twid ? std::strtoll(twid, nullptr, 10) : 0;
        if (id) paint_.set_box(id * 4, box_paint_of(*el, false, layer_opacity(id)));
        for (const auto& child : el->children()) self(self, child, id);
    };
    if (doc && doc->root()) visit(visit, doc->root(), 0);
    if (paint_.end_scan()) motion_stale_ = true;
#endif
}

void Renderer::recount_boxes(const std::vector<int64_t>& ids) {
#ifdef TW_LITEHTML_TWIDGET
    if (!paint_on_ || paint_stopped_) return;
    for (int64_t id : ids) {
        // Only a box the last count found: one under display: none stays out.
        auto el = container_->element(id);
        if (el && paint_.has_box(id * 4)) paint_.set_box(id * 4, box_paint_of(*el, false, layer_opacity(id)));
    }
    count_paint();
#else
    (void)ids;
#endif
}

bool Renderer::sync_motion(double now) {
    uint64_t hash = animator_.heavy_motion(now, nullptr);
    if (!motion_stale_ && hash == motion_hash_) return false;
    motion_stale_ = false;
    motion_hash_ = hash;
    animator_.heavy_motion(now, &motions_);
    std::vector<CountedMotion> running;
    running.reserve(motions_.size());
    for (const Animator::Motion& m : motions_) {
        std::string id(1, m.kind);
        id += ':';
        if (m.kind == 'w') id += std::to_string(m.aid);
        else id += m.name;
        running.push_back({m.id * 4, std::move(id), m.paint});
    }
    bool ended = false;
    for (const CountedMotion& c : motion_counted_) {
        if (std::any_of(running.begin(), running.end(), [&](const CountedMotion& r) { return r.key == c.key && r.id == c.id; })) continue;
        paint_.end_animation(c.key, c.id);
        ended = true;
    }
    for (const CountedMotion& r : running)
        if (!paint_.animating(r.key, r.id)) paint_.start_animation(r.key, r.id, r.paint);
    motion_counted_ = std::move(running);
    return ended;
}

void Renderer::count_paint() {
    if (!paint_on_ || still()) return;
    double now = now_s();
    // An animation that ended leaves its box's own values, last read while it applied: every box is read again.
    if (sync_motion(now)) paint_scan_ = true;
    if (paint_scan_) {
        paint_scan_ = false;
        scan_boxes();
        // A box that went took its animations; those that still run come back with it.
        sync_motion(now);
    }
    if (paint_reports_ && paint_reported_ != std::make_pair(paint_.heavy(), paint_.backdrop())) {
        paint_reported_ = {paint_.heavy(), paint_.backdrop()};
        pending_events_.push_back(Json::object().set("type", "paint").set("heavy", paint_.heavy()).set("backdrop", paint_.backdrop()));
    }
    if (auto verdict = paint_.check()) stop_paint(*verdict);
}

void Renderer::stop_layout(uint64_t boxes) {
    if (layout_stopped_) return;
    layout_stopped_ = true;
    // The host logs it (renderer-violation); the widget is frozen here with the badge alone (build_document).
    pending_violations_.push_back("layout limit: more than " + std::to_string(boxes - 1) + " boxes in one layout");
    freeze("Layout limit exceeded", true);
    // A build in progress rebuilds as the badge itself; a patch or a restyle falls back to one.
    stale_ = true;
}

void Renderer::stop_paint(const paint::Verdict& verdict) {
    // Reported before the frame is drawn; the host stops the widget and freezes the renderer with its own label.
    paint_stopped_ = true;
    pending_events_.push_back(Json::object().set("type", "limit").set("code", twidget::paintLimitCode(verdict.code)).set("count", verdict.count));
    // From this frame on: no blurred shadows, filters, backdrop-filter or transparency, animations stopped where they
    // are and the layers back in the document, so the stopped widget costs nothing to scroll. Running transitions are
    // cancelled first, as widget-sandbox's renderer does (renderer-bundle 0.6.3, zn()): each property shows its end
    // value, so a box fading out is hidden (opacity 0) instead of drawn opaque at the step it had reached.
    animator_.cancel_transitions();
    container_->set_no_effects();
    double now = now_s();
    update_composition(now);
#ifdef TW_LITEHTML_TWIDGET
    apply_overrides(animator_.sample(now));
#endif
    stale_ = false;
    pending_moves_.clear();
    dirty_ = touched_all_ = true;
    composition_only_ = false;
    painted_valid_ = false;
}

// ───────────────────────── accessibility ─────────────────────────

namespace {

/// Runs of white space as one space, none at the ends.
std::string collapse_space(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f') {
            space = !out.empty();
            continue;
        }
        if (space) out.push_back(' ');
        space = false;
        out.push_back(c);
    }
    return out;
}

std::string attr_of(const litehtml::element::ptr& el, const char* name) {
    const char* v = el->get_attr(name);
    return v ? css::trim(v) : std::string();
}

/// Where a text element's fragments are, document coordinates (Renderer::text_runs' boxes, joined).
bool text_box(const litehtml::element::ptr& el, Rect& out) {
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    auto add = [&](const litehtml::position& b) {
        if (float(b.width) <= 0 && float(b.height) <= 0) return;
        x0 = std::min(x0, float(b.x)); y0 = std::min(y0, float(b.y));
        x1 = std::max(x1, float(b.x + b.width)); y1 = std::max(y1, float(b.y + b.height));
    };
    el->for_each_render_item([&](const std::shared_ptr<litehtml::render_item>& ri) {
        litehtml::position placed = ri->get_placement();
        auto item = std::dynamic_pointer_cast<litehtml::render_text>(ri);
        if (item && !item->fragments.empty()) {
            for (const auto& fragment : item->fragments) {
                litehtml::position box = fragment->pos();
                box.x += placed.x - ri->pos().x;
                box.y += placed.y - ri->pos().y;
                add(box);
            }
        } else {
            add(placed);
        }
    });
    if (!std::isfinite(x0)) return false;
    out = Rect{x0, y0, x1 - x0, y1 - y0};
    return true;
}

Rect join(const Rect& a, const Rect& b, bool first) {
    if (first) return b;
    float x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    return Rect{x0, y0, std::max(a.x + a.w, b.x + b.w) - x0, std::max(a.y + a.h, b.y + b.h) - y0};
}

/// Not drawn, so not read: display none, visibility hidden, opacity 0, aria-hidden.
bool unseen(const litehtml::element::ptr& el) {
    const auto& c = el->css();
    if (c.get_display() == litehtml::display_none || c.get_visibility() != litehtml::visibility_visible || c.get_opacity() <= 0) return true;
    return attr_of(el, "aria-hidden") == "true";
}

/// Text in its own box: a block, an inline block, or a flex or grid item.
bool own_box(const litehtml::element::ptr& el) {
    if (!el->is_inline() || el->is_inline_box()) return true;
    auto parent = el->parent();
    if (!parent) return true;
    auto d = parent->css().get_display();
    return d == litehtml::display_flex || d == litehtml::display_inline_flex || d == litehtml::display_grid || d == litehtml::display_inline_grid;
}

std::string tag_of(const litehtml::element::ptr& el) {
    const char* t = el->get_tagName();
    return t ? lower(t) : std::string();
}

} // namespace

const char* Renderer::role_name(AccessNode::Role role) {
    static const char* names[] = {"text", "heading", "button", "link", "image", "checkbox", "switch", "radio", "tab", "slider", "progress", "textfield"};
    return names[role];
}

std::vector<Renderer::AccessNode> Renderer::accessibility() const {
    std::vector<AccessNode> out;
#ifdef TW_LITEHTML_TWIDGET
    litehtml::document* doc = container_->doc();
    if (!doc || !doc->root()) return out;
    using Role = AccessNode::Role;

    // The role an element has on its own (its `role`, else its tag's), with its level and states.
    struct Kind {
        bool any = false;
        Role role = AccessNode::Text;
        int level = 0;
        uint32_t flags = 0;
        bool decorative = false;  // an image with alt="" or role none: nothing to read
    };
    auto kind_of = [&](const litehtml::element::ptr& el) {
        Kind k;
        std::string tag = tag_of(el), role = lower(attr_of(el, "role"));
        auto set = [&](Role r) {
            k.any = true;
            k.role = r;
        };
        if (role == "presentation" || role == "none") {
            k.decorative = tag == "img" || tag == "svg";
            return k;
        }
        if (role == "button" || role == "menuitem" || role == "option") set(AccessNode::Button);
        else if (role == "link") set(AccessNode::Link);
        else if (role == "checkbox" || role == "menuitemcheckbox") set(AccessNode::Checkbox);
        else if (role == "switch") set(AccessNode::Switch);
        else if (role == "radio" || role == "menuitemradio") set(AccessNode::Radio);
        else if (role == "tab") set(AccessNode::Tab);
        else if (role == "heading") set(AccessNode::Heading);
        else if (role == "img" || role == "image") set(AccessNode::Image);
        else if (role == "slider") set(AccessNode::Slider);
        else if (role == "progressbar" || role == "meter") set(AccessNode::Progress);
        else if (role == "textbox" || role == "searchbox") set(AccessNode::TextField);
        else if (tag == "button") set(AccessNode::Button);
        else if (tag == "a" && el->get_attr("href")) set(AccessNode::Link);
        else if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
            set(AccessNode::Heading);
            k.level = tag[1] - '0';
        } else if (tag == "img") {
            set(AccessNode::Image);
            k.decorative = el->get_attr("alt") && attr_of(el, "alt").empty();
        } else if (tag == "summary" && el->parent() && tag_of(el->parent()) == "details") {
            set(AccessNode::Button);
            k.flags |= AccessNode::Expandable | (el->parent()->get_attr("open") ? AccessNode::Expanded : 0);
        } else if (tag == "progress" || tag == "meter") set(AccessNode::Progress);
        else if (tag == "textarea") set(AccessNode::TextField);
        else if (tag == "select") set(AccessNode::Button);
        else if (tag == "input") {
            std::string type = lower(attr_of(el, "type"));
            if (type == "hidden") k.decorative = true;
            else if (type == "checkbox") set(AccessNode::Checkbox);
            else if (type == "radio") set(AccessNode::Radio);
            else if (type == "range") set(AccessNode::Slider);
            else if (type == "button" || type == "submit" || type == "reset" || type == "image") set(AccessNode::Button);
            else set(AccessNode::TextField);
            if ((type == "checkbox" || type == "radio")) k.flags |= AccessNode::Checkable | (el->get_attr("checked") ? AccessNode::Checked : 0);
        }
        if (k.role == AccessNode::Heading && k.level == 0) {
            int level = std::atoi(attr_of(el, "aria-level").c_str());
            k.level = level >= 1 && level <= 6 ? level : 2;
        }
        // States, whichever role names them.
        auto tristate = [&](const char* name) {
            std::string v = lower(attr_of(el, name));
            if (v == "true") k.flags |= AccessNode::Checkable | AccessNode::Checked;
            else if (v == "mixed") k.flags |= AccessNode::Checkable | AccessNode::Mixed;
            else if (v == "false") k.flags |= AccessNode::Checkable;
        };
        tristate("aria-checked");
        tristate("aria-pressed");
        if (lower(attr_of(el, "aria-selected")) == "true") k.flags |= AccessNode::Selected;
        std::string expanded = lower(attr_of(el, "aria-expanded"));
        if (expanded == "true") k.flags |= AccessNode::Expandable | AccessNode::Expanded;
        else if (expanded == "false") k.flags |= AccessNode::Expandable;
        if (lower(attr_of(el, "aria-disabled")) == "true" || ((tag == "button" || tag == "input" || tag == "select" || tag == "textarea") && el->get_attr("disabled")))
            k.flags |= AccessNode::Disabled;
        return k;
    };
    auto actionable = [](Role r) {
        return r == AccessNode::Button || r == AccessNode::Link || r == AccessNode::Checkbox || r == AccessNode::Switch || r == AccessNode::Radio ||
               r == AccessNode::Tab || r == AccessNode::Slider || r == AccessNode::TextField;
    };

    // The text an element's content gives it, as a browser names a button from it: images by their alt or label,
    // a separate box apart from its neighbours.
    std::function<void(const litehtml::element::ptr&, std::string&)> text_of = [&](const litehtml::element::ptr& el, std::string& s) {
        if (el->is_text()) {
            el->get_text(s);
            return;
        }
        if (unseen(el)) return;
        std::string tag = tag_of(el);
        if (tag == "br") {
            s.push_back(' ');
            return;
        }
        std::string label = attr_of(el, "aria-label");
        if (label.empty() && tag == "img") label = attr_of(el, "alt");
        if (label.empty() && tag == "svg") {
            for (const auto& c : el->children())
                if (!c->is_text() && tag_of(c) == "title") c->get_text(label);
            label = collapse_space(label);
        }
        bool box = own_box(el);
        if (box) s.push_back(' ');
        if (!label.empty() || tag == "img" || tag == "canvas" || tag == "svg") {
            s += label;
        } else {
            for (const auto& c : el->children()) text_of(c, s);
        }
        if (box) s.push_back(' ');
    };
    // An element no role names counts as a button by an outermost cursor: pointer, unless it holds controls of its own.
    std::function<bool(const litehtml::element::ptr&)> holds_controls = [&](const litehtml::element::ptr& el) {
        for (const auto& c : el->children()) {
            if (c->is_text() || unseen(c)) continue;
            Kind k = kind_of(c);
            if (k.any && actionable(k.role)) return true;
            if (holds_controls(c)) return true;
        }
        return false;
    };
    auto pointer_button = [&](const litehtml::element::ptr& el) {
        std::string tag = tag_of(el);
        if (tag == "html" || tag == "body" || el->css().get_cursor() != "pointer") return false;
        auto parent = el->parent();
        if (parent && parent->css().get_cursor() == "pointer") return false;
        return !holds_controls(el);
    };
    auto number = [](const std::string& s, double& v) {
        if (s.empty()) return false;
        char* end = nullptr;
        v = std::strtod(s.c_str(), &end);
        return end != s.c_str() && std::isfinite(v);
    };
    auto value_of = [&](const litehtml::element::ptr& el, Role role, int64_t id) -> std::string {
        std::string text = attr_of(el, "aria-valuetext");
        if (!text.empty()) return text;
        std::string tag = tag_of(el);
        if (role == AccessNode::TextField) {
            const Node* n = node(id);
            return n ? control_value(n) : std::string();
        }
        bool native = tag == "progress" || tag == "meter" || tag == "input";
        double now, lo = 0, hi = native && tag != "input" ? 1 : 100;
        if (!number(attr_of(el, native ? "value" : "aria-valuenow"), now)) return {};
        number(attr_of(el, native ? "min" : "aria-valuemin"), lo);
        number(attr_of(el, native ? "max" : "aria-valuemax"), hi);
        if (role == AccessNode::Slider) return Json::number(now);
        if (hi <= lo) return {};
        return std::to_string(int(std::lround(std::clamp((now - lo) / (hi - lo), 0.0, 1.0) * 100))) + "%";
    };
    // A tap at the box's centre, or else at another point of it, that lands on the element or inside it.
    auto place_tap = [&](AccessNode& a) {
        const Rect& b = a.box;
        static const float points[][2] = {{.5f, .5f}, {.25f, .25f}, {.75f, .25f}, {.25f, .75f}, {.75f, .75f}, {.5f, .25f}, {.5f, .75f}, {.25f, .5f}, {.75f, .5f}};
        for (const auto& p : points) {
            float x = b.x + b.w * p[0], y = b.y + b.h * p[1];
            int64_t hit = widget_id_at(x, y, nullptr);
            if (hit == a.id || is_ancestor(a.id, hit)) {
                a.tap_x = x;
                a.tap_y = y;
                return;
            }
        }
        a.tap_x = b.x + b.w / 2;
        a.tap_y = b.y + b.h / 2;
        a.flags |= AccessNode::Unreachable;
    };

    // The text of the block being walked, read as one node when the block ends or a node of another role cuts it.
    struct Block {
        int64_t id = 0;
        std::string text;
        Rect box;
        bool boxed = false;
        bool live = false;
    };
    auto flush = [&](Block& b) {
        std::string label = collapse_space(b.text);
        if (!label.empty() && b.boxed) {
            AccessNode a;
            a.role = AccessNode::Text;
            a.id = b.id;
            a.box = b.box;
            a.label = std::move(label);
            if (b.live) a.flags |= AccessNode::Live;
            out.push_back(std::move(a));
        }
        b.text.clear();
        b.boxed = false;
    };
    std::function<void(const litehtml::element::ptr&, Block&, bool, bool)> walk = [&](const litehtml::element::ptr& el, Block& block, bool live, bool svg) {
        if (el->is_text()) {
            std::string t;
            el->get_text(t);
            block.text += t;
            Rect r;
            if (collapse_space(t).empty() || !text_box(el, r)) return;
            auto parent = el->parent();
            r = parent ? transformed_box(parent, r) : r;
            block.box = join(block.box, r, !block.boxed);
            block.boxed = true;
            return;
        }
        if (unseen(el)) return;
        std::string tag = tag_of(el);
        if (tag == "head" || tag == "style" || tag == "title" || tag == "desc") return;
        std::string live_attr = lower(attr_of(el, "aria-live")), role_attr = lower(attr_of(el, "role"));
        if (!live_attr.empty()) live = live_attr != "off";
        else if (role_attr == "status" || role_attr == "alert" || role_attr == "log" || role_attr == "timer") live = true;
        Kind k = kind_of(el);
        if (k.decorative) return;
        bool by_cursor = !k.any && !svg && pointer_button(el);
        if (by_cursor) {
            k.any = true;
            k.role = AccessNode::Button;
        }
        int64_t id = container_->widget_id(el, nullptr);
        if (k.any) {
            AccessNode a;
            a.role = k.role;
            a.id = id;
            a.level = k.level;
            a.flags = k.flags | (live ? AccessNode::Live : 0);
            a.box = visual_box_of(el);
            a.label = collapse_space(attr_of(el, "aria-label"));
            if (a.label.empty() && tag == "img") a.label = collapse_space(attr_of(el, "alt"));
            if (a.label.empty() && tag == "svg") {
                std::string t;
                text_of(el, t);
                a.label = collapse_space(t);
            }
            if (a.label.empty() && a.role == AccessNode::TextField) a.label = collapse_space(attr_of(el, "placeholder"));
            if (a.label.empty() && tag == "input" && a.role == AccessNode::Button) a.label = collapse_space(attr_of(el, "value"));
            if (a.label.empty() && a.role != AccessNode::Image && a.role != AccessNode::TextField && a.role != AccessNode::Slider && a.role != AccessNode::Progress) {
                std::string t;
                for (const auto& c : el->children()) text_of(c, t);
                a.label = collapse_space(t);
            }
            if (a.label.empty()) a.label = collapse_space(attr_of(el, "title"));
            if (a.role == AccessNode::Slider || a.role == AccessNode::Progress || a.role == AccessNode::TextField) a.value = value_of(el, a.role, id);
            // Nothing to read and nothing to do (an unlabelled image, an empty box) is left out. A pointer cursor
            // with no name (a drag area: a seek bar, a dimmer) is no button.
            bool empty = a.label.empty() && a.value.empty() && !actionable(a.role) && a.role != AccessNode::Progress;
            if (!(by_cursor && a.label.empty())) {
                if (empty || (a.box.w <= 0 && a.box.h <= 0)) return;
                flush(block);
                if (actionable(a.role)) place_tap(a);
                out.push_back(std::move(a));
                return;
            }
        }
        // An SVG drawing or a canvas no role names: only an SVG's <text> is read.
        if (tag == "canvas") return;
        bool inner_svg = svg || tag == "svg";
        if (inner_svg && tag == "text") {
            std::string t;
            text_of(el, t);
            AccessNode a;
            a.id = id;
            a.box = visual_box_of(el);
            a.label = collapse_space(t);
            a.flags = live ? AccessNode::Live : 0;
            flush(block);
            if (!a.label.empty() && (a.box.w > 0 || a.box.h > 0)) out.push_back(std::move(a));
            return;
        }
        if (!inner_svg && own_box(el)) {
            flush(block);
            Block mine;
            mine.id = id;
            mine.live = live;
            for (const auto& c : el->children()) walk(c, mine, live, inner_svg);
            flush(mine);
            return;
        }
        for (const auto& c : el->children()) walk(c, block, live, inner_svg);
    };
    Block root;
    walk(doc->root(), root, false, false);
    flush(root);
#endif
    return out;
}

const std::string& Renderer::accessibility_json() const {
#ifdef TW_LITEHTML_TWIDGET
    litehtml::document* doc = container_->doc();
    auto [painted, generation, layouts] = container_->paint_key();
    auto key = std::make_tuple(painted, generation, layouts, doc ? doc->style_epoch() : 0);
    if (key == access_key_) return access_json_;
    access_key_ = key;
    double t0 = now_ms();
    std::string s = "[";
    for (const AccessNode& a : accessibility()) {
        if (s.size() > 1) s.push_back(',');
        s += "{\"r\":\"" + std::string(role_name(a.role)) + "\",\"id\":" + std::to_string(a.id) + ",\"b\":[" + Json::number(a.box.x) + "," +
             Json::number(a.box.y) + "," + Json::number(a.box.w) + "," + Json::number(a.box.h) + "]";
        if (!a.label.empty()) s += ",\"l\":" + Json::quote(a.label);
        if (!a.value.empty()) s += ",\"v\":" + Json::quote(a.value);
        if (a.level) s += ",\"lv\":" + std::to_string(a.level);
        if (a.flags) s += ",\"s\":" + std::to_string(a.flags);
        if (a.tap_x || a.tap_y) s += ",\"t\":[" + Json::number(a.tap_x) + "," + Json::number(a.tap_y) + "]";
        s.push_back('}');
    }
    s.push_back(']');
    if (s != access_json_) {
        access_json_ = std::move(s);
        access_version_++;
    }
    access_builds_++;
    access_ms_ += now_ms() - t0;
#endif
    return access_json_;
}

uint64_t Renderer::accessibility_version() const {
    accessibility_json();
    return access_version_;
}

// ───────────────────────── text selection ─────────────────────────

const std::vector<Renderer::TextRun>& Renderer::text_runs() const {
#ifdef TW_LITEHTML_TWIDGET
    litehtml::document* doc = container_->doc();
    auto [painted, generation, layouts] = container_->paint_key();
    auto key = std::make_tuple(painted, generation, layouts, doc ? doc->style_epoch() : 0);
    if (key == runs_key_) return runs_;
    runs_key_ = key;
    std::vector<TextRun> before = std::move(runs_);
    runs_.clear();
    std::vector<TextRun>& runs = runs_;
    if (!doc || !doc->root()) return runs;
    std::unordered_map<int64_t, size_t> lengths;  // each owner's text so far
    int blocks = 0;
    // user-select is not inherited, but `auto` follows the parent: the nearest element that says text or none decides.
    auto selectable = [](const litehtml::element* el) {
        for (; el; el = el->parent().get()) {
            auto value = el->css().get_user_select();
            if (value != litehtml::user_select_auto) return value == litehtml::user_select_text;
        }
        return false;
    };
    auto add = [&](int64_t owner, int block, std::string text, const litehtml::position* box, uintptr_t font, bool can) {
        if (text.empty()) return;
        TextRun r;
        r.owner = owner;
        r.offset = lengths[owner];
        r.block = block;
        r.font = font;
        r.selectable = can;
        r.line_break = !box;
        if (box) r.box = Rect{float(box->x), float(box->y), float(box->width), float(box->height)};
        lengths[owner] += text.size();
        r.text = std::move(text);
        runs.push_back(std::move(r));
    };
    std::function<void(const litehtml::element::ptr&, int64_t, int)> walk = [&](const litehtml::element::ptr& el, int64_t owner, int block) {
        if (!el->is_text()) {
            if (el->css().get_display() == litehtml::display_none) return;
            if (const char* id = el->get_attr("data-twid")) owner = std::strtoll(id, nullptr, 10);
            if (!el->is_inline()) block = ++blocks;
            if (dynamic_cast<const litehtml::el_break*>(el.get())) {
                add(owner, block, "\n", nullptr, 0, selectable(el.get()));
                return;
            }
            for (const auto& child : el->children()) walk(child, owner, block);
            return;
        }
        auto text = std::dynamic_pointer_cast<litehtml::el_text>(el);
        auto parent = el->parent();
        if (!text || !parent) return;
        bool can = selectable(parent.get());
        uintptr_t font = uintptr_t(parent->css().get_font());
        const std::string& shown = text->display_text();
        el->for_each_render_item([&](const std::shared_ptr<litehtml::render_item>& ri) {
            litehtml::position placed = ri->get_placement();
            auto item = std::dynamic_pointer_cast<litehtml::render_text>(ri);
            if (item && !item->fragments.empty()) {
                // A fragment's box is in its item's frame.
                for (const auto& fragment : item->fragments) {
                    litehtml::position box = fragment->pos();
                    box.x += placed.x - ri->pos().x;
                    box.y += placed.y - ri->pos().y;
                    add(owner, block, fragment->fragment_text, &box, font, can);
                }
            } else {
                add(owner, block, item && item->cut >= 0 ? shown.substr(0, size_t(item->cut)) : shown, &placed, font, can);
            }
        });
    };
    walk(doc->root(), 0, 0);
    // A restyle that moved nothing (a press's :active) keeps where the last paint drew each run, which is
    // what shows until the next one, and the carets measured in it.
    if (before.size() == runs.size()) {
        for (size_t i = 0; i < runs.size(); i++) {
            TextRun& r = runs[i];
            TextRun& old = before[i];
            if (r.owner != old.owner || r.offset != old.offset || r.font != old.font || r.text != old.text || std::memcmp(&r.box, &old.box, sizeof(Rect)) != 0) continue;
            r.shown = old.shown;
            r.layer = old.layer;
            r.drawn = old.drawn;
            r.advances = std::move(old.advances);
        }
    }
#endif
    return runs_;
}

bool Renderer::run_shown(const TextRun& r, double now, std::unordered_map<int64_t, std::optional<Affine>>& layers, Affine& out) const {
    out = {};
    if (!r.drawn) return true;
    out = r.shown;
    if (!r.layer) return true;
    auto it = layers.find(r.layer);
    if (it == layers.end()) {
        Affine shown;
        float opacity = 1;
        it = layers.emplace(r.layer, layer_shown(r.layer, now, shown, opacity) ? std::optional<Affine>(shown) : std::nullopt).first;
    }
    if (!it->second) return false;
    out = *it->second * out;
    return true;
}

std::string Renderer::selectable_text_json() const {
    Json out = Json::array();
    std::unordered_map<int64_t, std::optional<Affine>> layers;
    double now = now_s();
    for (const TextRun& r : text_runs()) {
        if (!r.selectable) continue;
        int font = 0;
        float descent = 0, letter = 0, word = 0;
        if (!r.line_break && !container_->font_info(r.font, font, descent, letter, word)) continue;
        Json run = Json::array();
        run.push(r.block).push(r.box.x).push(r.box.y).push(r.box.w).push(r.box.h);
        // Where the run is drawn: draw_text puts the baseline `descent` above the bottom of its box.
        run.push(r.line_break ? 0.0f : r.box.y + r.box.h - descent).push(font).push(letter).push(word).push(r.text);
        // Where it shows: the transforms around it as last painted, after its layer's track as it is now.
        // A run whose layer is hidden keeps its place in the text, at its layout position.
        Affine m;
        if (!run_shown(r, now, layers, m)) m = {};
        run.push(Json::array().push(m.a).push(m.b).push(m.c).push(m.d).push(m.e).push(m.f));
        out.push(std::move(run));
    }
    return out.dump();
}

bool Renderer::locate(const TextPoint& p, const std::vector<TextRun>& runs, size_t& run, size_t& offset) const {
    bool found = false;
    for (size_t i = 0; i < runs.size(); i++) {
        const TextRun& r = runs[i];
        if (r.owner != p.owner || p.offset < r.offset || p.offset > r.offset + r.text.size()) continue;
        run = i;
        offset = p.offset - r.offset;
        found = true;
        // The end of a run is also the start of the owner's next one: that one takes it.
        if (offset < r.text.size()) return true;
    }
    return found;
}

float Renderer::run_advance(const TextRun& run, size_t bytes) const {
    if (run.line_break || bytes == 0) return 0;
    if (bytes >= run.text.size()) return run.box.w;
    if (run.text.size() > MAX_MEASURED_RUN) return run.box.w * float(bytes) / float(run.text.size());
    if (auto it = run.advances.find(bytes); it != run.advances.end()) return it->second;
    float w = float(container_->text_width(run.text.substr(0, bytes).c_str(), litehtml::uint_ptr(run.font)));
    run.advances.emplace(bytes, w);
    return w;
}

bool Renderer::point_at(double x, double y, const std::vector<TextRun>& runs, TextPoint& out) const {
    const TextRun* best = nullptr;
    double best_dy = 0, best_dx = 0, best_x = 0;
    // The point in each run's own coordinates: back through the layer's track as it shows now, then the
    // groups' transforms around the run.
    std::unordered_map<int64_t, std::optional<Affine>> layers;
    double now = now_s();
    for (const TextRun& r : runs) {
        if (r.line_break) continue;
        double rx = x, ry = y;
        Affine m, inverse;
        if (!run_shown(r, now, layers, m)) continue;
        if (!(m == Affine{})) {
            if (!m.invert(inverse)) continue;
            rx = inverse.apply_x(float(x), float(y));
            ry = inverse.apply_y(float(x), float(y));
        }
        double dy = ry < r.box.y ? r.box.y - ry : ry > r.box.y + r.box.h ? ry - (r.box.y + r.box.h) : 0;
        double dx = rx < r.box.x ? r.box.x - rx : rx > r.box.x + r.box.w ? rx - (r.box.x + r.box.w) : 0;
        if (!best || dy < best_dy || (dy == best_dy && dx < best_dx)) {
            best = &r;
            best_dy = dy;
            best_dx = dx;
            best_x = rx;
        }
    }
    if (!best) return false;
    const std::string& text = best->text;
    auto inside_sequence = [&](size_t i) { return i < text.size() && (uint8_t(text[i]) & 0xC0) == 0x80; };
    double target = best_x - best->box.x;
    size_t bytes = 0;
    if (text.size() > MAX_MEASURED_RUN) {
        // Evenly spread, as run_advance spreads them.
        double share = best->box.w > 0 ? std::clamp(target / best->box.w, 0.0, 1.0) : 0.0;
        bytes = size_t(std::lround(share * double(text.size())));
        while (inside_sequence(bytes)) bytes--;
    } else {
        // The character boundaries' advances grow along the run: the first one at or past the point, or
        // the one before it when that is closer.
        std::vector<size_t> stops{0};
        for (size_t i = 1; i <= text.size(); i++)
            if (!inside_sequence(i)) stops.push_back(i);
        size_t lo = 0, hi = stops.size() - 1;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (run_advance(*best, stops[mid]) < target) lo = mid + 1;
            else hi = mid;
        }
        bytes = stops[lo];
        if (lo > 0 && target - run_advance(*best, stops[lo - 1]) <= run_advance(*best, stops[lo]) - target) bytes = stops[lo - 1];
    }
    out = TextPoint{best->owner, best->offset + bytes};
    return true;
}

bool Renderer::selection_range(const std::vector<TextRun>& runs, size_t& first, size_t& first_offset, size_t& last, size_t& last_offset) const {
    if (selection_.anchor == selection_.focus) return false;
    size_t ar = 0, ao = 0, fr = 0, fo = 0;
    if (!locate(selection_.anchor, runs, ar, ao) || !locate(selection_.focus, runs, fr, fo)) return false;
    if (std::tie(ar, ao) > std::tie(fr, fo)) {
        std::swap(ar, fr);
        std::swap(ao, fo);
    }
    first = ar;
    first_offset = ao;
    last = fr;
    last_offset = fo;
    return true;
}

bool Renderer::starts_selection(int64_t target) const {
#ifdef TW_LITEHTML_TWIDGET
    // A listener that prevents the press keeps it, as preventDefault on mousedown does in a browser.
    for (const Node* n = node(target); n; n = n->parent) {
        std::string id = std::to_string(n->id);
        if (prevent_.count(id + ":pointerdown") || prevent_.count(id + ":mousedown")) return false;
    }
    for (auto el = container_->element(target); el; el = el->parent()) {
        auto value = el->css().get_user_select();
        if (value != litehtml::user_select_auto) return value == litehtml::user_select_text;
    }
#endif
    return false;
}

std::string Renderer::selected_text() const {
    const auto& runs = text_runs();
    size_t first = 0, first_offset = 0, last = 0, last_offset = 0;
    if (!selection_range(runs, first, first_offset, last, last_offset)) return {};
    auto space = [](char c) { return c == ' ' || c == '\n' || c == '\t'; };
    std::string out;
    const TextRun* previous = nullptr;
    for (size_t i = first; i <= last; i++) {
        const TextRun& r = runs[i];
        size_t from = i == first ? first_offset : 0, to = i == last ? last_offset : r.text.size();
        if (!r.selectable || from >= to) continue;
        if (previous && !out.empty() && out.back() != '\n' && !r.line_break) {
            // Blocks apart by a line break; the lines of one block by a space, where a wrap ate the one between.
            if (r.block != previous->block) out += '\n';
            else if (r.box.y >= previous->box.y + previous->box.h / 2 && !space(out.back()) && !space(r.text[from])) out += ' ';
        }
        out.append(r.text, from, to - from);
        previous = &r;
    }
    return out;
}

bool Renderer::has_selection() const { return !selected_text().empty(); }

bool Renderer::clear_selection() {
    bool had = !(selection_.anchor == selection_.focus);
    selection_ = {};
    if (had) selection_changed();
    return had;
}

void Renderer::selection_changed() { dirty_ = touched_all_ = true; }

void Renderer::restyled_by_input() {
    // :hover and :active restyle without a message: the transitions they start begin now, not with the next batch.
    dirty_ = touched_all_ = paint_scan_ = true;
    animate();
}

bool Renderer::selectable_text() const {
#ifdef TW_LITEHTML_TWIDGET
    litehtml::document* doc = container_->doc();
    std::pair<const void*, uint64_t> key{doc, doc ? doc->style_epoch() : 0};
    if (key == selectable_key_) return selectable_;
    selectable_key_ = key;
    selectable_ = false;
    std::function<void(const litehtml::element::ptr&)> visit = [&](const litehtml::element::ptr& el) {
        if (selectable_ || el->is_text() || el->css().get_display() == litehtml::display_none) return;
        if (el->css().get_user_select() == litehtml::user_select_text) selectable_ = true;
        for (const auto& child : el->children()) visit(child);
    };
    if (doc && doc->root()) visit(doc->root());
    return selectable_;
#else
    return false;
#endif
}

void Renderer::paint_selection(DisplayList& list, int64_t layer, bool tint) const {
    if (!selectable_text()) return;
    const auto& runs = text_runs();
    // A run's Text command starts where its box does, in the coordinates of the groups around it.
    std::unordered_map<long, std::vector<size_t>> rows;
    for (size_t i = 0; i < runs.size(); i++)
        if (runs[i].selectable && !runs[i].line_break) rows[long(std::floor(runs[i].box.y))].push_back(i);
    if (rows.empty()) return;
    auto run_at = [&](const Rect& drawn) -> const TextRun* {
        long below = long(std::floor(drawn.y - 0.05f)), above = long(std::floor(drawn.y + 0.05f));
        for (long row = below; row <= above; row++) {
            auto it = rows.find(row);
            if (it == rows.end()) continue;
            for (size_t i : it->second) {
                const TextRun& r = runs[i];
                if (std::fabs(r.box.x - drawn.x) < 0.05f && std::fabs(r.box.y - drawn.y) < 0.05f) return &r;
            }
        }
        return nullptr;
    };
    size_t first = 0, first_offset = 0, last = 0, last_offset = 0;
    tint = tint && selection_range(runs, first, first_offset, last, last_offset);
    // Over the text, in the theme's link colour: a tint the text shows through, on every painter.
    Color color{51, 144, 236, 255};
    if (tint) {
        for (const auto& [name, value] : theme_vars_)
            if (name == "--tg-theme-link-color") css::parse_color(value, color);
        color.a = uint8_t(std::lround(color.a * 0.3));
    }
    std::vector<Affine> frames{Affine{}};
    std::vector<std::pair<size_t, Command>> tints;
    std::unordered_set<const TextRun*> placed;
    for (size_t i = 0; i < list.size(); i++) {
        const Command& c = list[i];
        if (c.kind == Cmd::GroupPush) {
            frames.push_back(frames.back() * Affine{c.matrix[0], c.matrix[1], c.matrix[2], c.matrix[3], c.matrix[4], c.matrix[5]});
            continue;
        }
        if (c.kind == Cmd::GroupPop) {
            if (frames.size() > 1) frames.pop_back();
            continue;
        }
        if (c.kind != Cmd::Text) continue;
        const TextRun* r = run_at(c.rect);
        // Once: a run drawn again (a text shadow) has its tint after the first.
        if (!r || !placed.insert(r).second) continue;
        r->shown = frames.back();
        r->layer = layer;
        r->drawn = true;
        size_t k = size_t(r - runs.data());
        if (!tint || k < first || k > last) continue;
        size_t from = k == first ? first_offset : 0, to = k == last ? last_offset : r->text.size();
        if (from >= to) continue;
        float x0 = run_advance(*r, from), x1 = run_advance(*r, to);
        if (x1 <= x0) continue;
        Command t;
        t.kind = Cmd::FillRect;
        t.rect = Rect{r->box.x + x0, r->box.y, x1 - x0, r->box.h};
        t.color = color;
        tints.emplace_back(i + 1, std::move(t));
    }
    if (tints.empty()) return;
    DisplayList out;
    out.reserve(list.size() + tints.size());
    size_t next = 0;
    for (size_t i = 0; i <= list.size(); i++) {
        for (; next < tints.size() && tints[next].first == i; next++) out.push_back(std::move(tints[next].second));
        if (i < list.size()) out.push_back(std::move(list[i]));
    }
    list = std::move(out);
}

} // namespace twlite
