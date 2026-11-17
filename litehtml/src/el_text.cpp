#include "html.h"
#include "el_text.h"
#include "render_item.h"
#include "render_inline.h"
#include "document_container.h"
#include "vertical.h"

#include <cmath>

namespace
{
    size_t utf8_length(const std::string& s)
    {
        size_t n = 0;
        for(unsigned char c : s)
            if((c & 0xC0) != 0x80)
                n++;
        return n;
    }

    // text-emphasis (傍点): no mark for spaces and punctuation (CSS Text Decoration 3 §3.1, general category P,
    // approximated by blocks).
    bool emphasis_skips(char32_t c)
    {
        return c <= ' ' || (c < 0x80 && !((c | 0x20) >= 'a' && (c | 0x20) <= 'z') && !(c >= '0' && c <= '9')) || c == 0xA0 ||
               (c >= 0x2010 && c <= 0x2027) || (c >= 0x2030 && c <= 0x205E) ||
               (c >= 0x3000 && c <= 0x303F && c != 0x3005 && c != 0x3006 && c != 0x3007) || c == 0x30FB ||
               (c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) ||
               (c >= 0xFF5B && c <= 0xFF65);
    }

    // The mark text-emphasis-style names: filled or open dot, circle, double-circle, triangle, sesame, or a
    // string. A fill alone takes the writing mode's shape (circle across, sesame down), a shape alone is filled.
    std::string emphasis_mark(const std::string& style, bool vertical)
    {
        std::string s = litehtml::trim(style);
        if(s.empty() || s == "none")
            return {};
        if(s.front() == '"' || s.front() == '\'')
            return s.size() > 2 ? s.substr(1, s.size() - 2) : std::string();
        const bool open = s.find("open") != std::string::npos;
        std::string shape = s.find("double-circle") != std::string::npos ? "double-circle"
                          : s.find("circle") != std::string::npos       ? "circle"
                          : s.find("dot") != std::string::npos          ? "dot"
                          : s.find("triangle") != std::string::npos     ? "triangle"
                          : s.find("sesame") != std::string::npos       ? "sesame"
                          : vertical                                    ? "sesame"
                                                                        : "circle";
        if(shape == "dot")
            return open ? "\xE2\x97\xA6" : "\xE2\x80\xA2";
        if(shape == "circle")
            return open ? "\xE2\x97\x8B" : "\xE2\x97\x8F";
        if(shape == "double-circle")
            return open ? "\xE2\x97\x8E" : "\xE2\x97\x89";
        if(shape == "triangle")
            return open ? "\xE2\x96\xB3" : "\xE2\x96\xB2";
        return open ? "\xEF\xB9\x86" : "\xEF\xB9\x85";
    }

    // Draws a mark over (or under) each character of the run, centred on it along the line; in vertical text the
    // marks go on the right (over) and stand upright. Line boxes do not grow for them (they use the leading).
    void draw_emphasis(litehtml::document* doc, litehtml::uint_ptr hdc, const std::string& text,
                       const litehtml::css_properties& css, const litehtml::position& pos, bool vertical)
    {
        using namespace litehtml;
        const uint_ptr mark_font = css.get_font_emphasis();
        const std::string mark   = emphasis_mark(css.get_text_emphasis_style(), vertical);
        if(!mark_font || mark.empty() || css.combines_upright())
            return;
        auto*               container = doc->container();
        const font_metrics& fm        = css.get_font_metrics();
        const font_metrics& mm        = css.get_emphasis_metrics();
        const int           where     = css.get_text_emphasis_position();
        const bool          under     = vertical ? (where & text_emphasis_position_left) && !(where & text_emphasis_position_right)
                                                 : (where & text_emphasis_position_under) != 0;
        web_color color = css.get_text_emphasis_color();
        if(color == web_color::current_color)
            color = css.get_color();
        const pixel_t baseline = pos.y + pos.height - fm.descent;
        const pixel_t centre   = under ? baseline + fm.em_bottom() + mm.font_size / 2_px : baseline - fm.em_top() - mm.font_size / 2_px;
        const pixel_t mark_w   = container->text_width(mark.c_str(), mark_font);
        const pixel_t mark_top = centre + (mm.em_top() - mm.em_bottom()) / 2_px - mm.height + mm.descent;
        const bool    em_each  = vertical && css.get_text_orientation() == text_orientation_upright;
        for(size_t i = 0; i < text.size();)
        {
            const unsigned char c0 = text[i];
            size_t n = c0 < 0x80 ? 1 : (c0 >> 5) == 6 ? 2 : (c0 >> 4) == 14 ? 3 : 4;
            n        = std::min(n, text.size() - i);
            char32_t cp = n == 1 ? c0 : n == 2 ? c0 & 0x1F : n == 3 ? c0 & 0x0F : c0 & 0x07;
            for(size_t k = 1; k < n; k++)
                cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
            if(!emphasis_skips(cp))
            {
                pixel_t x0, w;
                if(em_each)
                {
                    x0 = fm.font_size * pixel_t(float(utf8_length(text.substr(0, i))));
                    w  = fm.font_size;
                } else
                {
                    x0 = i ? container->text_width(text.substr(0, i).c_str(), css.get_font()) : 0_px;
                    w  = container->text_width(text.substr(i, n).c_str(), css.get_font());
                }
                const pixel_t cx = pos.x + x0 + w / 2_px;
                position      at(cx - mark_w / 2_px, mark_top, mark_w, mm.height);
                if(vertical)
                {
                    paint_group group;
                    group.matrix = upright_matrix(float(cx), float(centre));
                    group.bounds = at;
                    container->begin_group(hdc, group);
                    container->draw_text(hdc, mark.c_str(), mark_font, color, at);
                    container->end_group(hdc);
                } else
                {
                    container->draw_text(hdc, mark.c_str(), mark_font, color, at);
                }
            }
            i += n;
        }
    }

    // Vertical text (document::vertical()): the page's turn already makes a run sideways, so runs of sideways
    // characters are drawn as horizontal text is; each upright character is turned back about its em box's centre
    // and drawn with the font's vertical forms. The em box's centre is the line's central baseline.
    void draw_vertical(litehtml::document* doc, litehtml::uint_ptr hdc, const std::string& text,
                       const litehtml::css_properties& css, litehtml::web_color color, const litehtml::position& pos)
    {
        using namespace litehtml;
        auto*               container   = doc->container();
        const uint_ptr      font        = css.get_font();
        if(css.combines_upright())
        {
            // 縦中横: the whole run upright in one em, squeezed across when wider (as Chromium fits 2 to 4
            // characters), centred on the column.
            const font_metrics& m  = css.get_font_metrics();
            const pixel_t       w  = container->text_width(text.c_str(), font);
            const pixel_t       cx = pos.x + pos.width / 2_px;
            const pixel_t       cy = pos.y + pos.height - m.descent - (m.em_top() - m.em_bottom()) / 2_px;
            const float         s  = w > m.font_size ? float(m.font_size / w) : 1.0f;
            paint_group         group;
            group.matrix = {0, -s, 1, 0, float(cx - cy), s * float(cx) + float(cy)};
            group.bounds = position(cx - w / 2_px, pos.y, w, pos.height);
            container->begin_group(hdc, group);
            container->draw_text(hdc, text.c_str(), font, color, position(cx - w / 2_px, pos.y, w, pos.height));
            container->end_group(hdc);
            return;
        }
        const uint_ptr      upright     = css.get_font_upright();
        const font_metrics& fm          = css.get_font_metrics();
        const auto          orientation = css.get_text_orientation();
        const pixel_t       baseline    = pos.y + pos.height - fm.descent;
        // Upright glyphs are centred on the central baseline, halfway between the ascent and descent the line makes
        // room for (the fonts the text is drawn with, fallback fonts too: text_used_extents, as line-height: normal
        // counts them), as Blink lays vertical text out on its central baseline. The primary font's em box alone
        // (Roboto, for Japanese drawn in Noto Sans CJK) put them about 3 CSS px off a browser's.
        pixel_t ascent = fm.ascent, descent = fm.descent;
        {
            pixel_t used_ascent = 0_px, used_descent = 0_px;
            if(container->text_used_extents(text.c_str(), font, used_ascent, used_descent))
            {
                ascent  = std::max(ascent, used_ascent);
                descent = std::max(descent, used_descent);
            }
        }
        const pixel_t       cy          = baseline - (ascent - descent) / 2_px;
        size_t              run         = 0;  // start of the sideways run not drawn yet
        // text-orientation: upright gives each character an em (el_text::compute_styles); mixed keeps the advances.
        const bool all_upright = orientation == text_orientation_upright;
        auto advance = [&](size_t end) {
            if(all_upright)
                return fm.font_size * pixel_t(float(utf8_length(text.substr(0, end))));
            return end ? container->text_width(text.substr(0, end).c_str(), font) : 0_px;
        };
        auto flush   = [&](size_t end) {
            if(end <= run)
                return;
            position at  = pos;
            at.x        += advance(run);
            at.width     = container->text_width(text.substr(run, end - run).c_str(), font);
            container->draw_text(hdc, text.substr(run, end - run).c_str(), font, color, at);
        };
        for(size_t i = 0; i < text.size();)
        {
            const unsigned char c0 = text[i];
            size_t n = c0 < 0x80 ? 1 : (c0 >> 5) == 6 ? 2 : (c0 >> 4) == 14 ? 3 : 4;
            n        = std::min(n, text.size() - i);
            char32_t cp = n == 1 ? c0 : n == 2 ? c0 & 0x1F : n == 3 ? c0 & 0x0F : c0 & 0x07;
            for(size_t k = 1; k < n; k++)
                cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
            const std::string glyph = text.substr(i, n);
            const bool up = orientation == text_orientation_upright ||
                            (orientation == text_orientation_mixed && upright_in_vertical(cp) &&
                             (!turned_in_vertical(cp) || container->has_vertical_form(glyph.c_str(), font)));
            if(up)
            {
                flush(i);
                const pixel_t     w     = container->text_width(glyph.c_str(), font);
                const pixel_t     cx    = pos.x + advance(i) + (all_upright ? fm.font_size : w) / 2_px;
                paint_group       group;
                group.matrix = upright_matrix(float(cx), float(cy));
                group.bounds = position(cx - w / 2_px, pos.y, w, pos.height);
                container->begin_group(hdc, group);
                container->draw_text(hdc, glyph.c_str(), upright, color, position(cx - w / 2_px, pos.y, w, pos.height));
                container->end_group(hdc);
                run = i + n;
            }
            i += n;
        }
        flush(text.size());
    }
} // namespace

namespace
{
    // A text node's style until compute_styles gives it its parent's text_css().
    const std::shared_ptr<litehtml::css_properties>& unstyled_text_css()
    {
        static const auto& css = *new std::shared_ptr<litehtml::css_properties>([] {
            auto c = std::make_shared<litehtml::css_properties>();
            c->set_display(litehtml::display_inline_text);
            return c;
        }());
        return css;
    }
} // namespace

litehtml::el_text::el_text(const char* text, const document::ptr& doc) :
    element(doc)
{
    if(text)
    {
        m_text = text;
    }
    m_use_transformed = false;
    m_draw_spaces     = true;
    m_css             = unstyled_text_css();
}

void litehtml::el_text::get_content_size(size& sz, pixel_t /*max_width*/)
{
    sz = m_size;
}

void litehtml::el_text::get_text(std::string& text) const
{
    text += m_text;
}

int litehtml::el_text::strong_direction() const
{
    if(m_strong_direction != 2)
        return m_strong_direction;
    m_strong_direction = 0;
    for(size_t i = 0; i < m_text.size();)
    {
        unsigned char c = m_text[i];
        if(c < 0x80)
        {
            if((c | 0x20) >= 'a' && (c | 0x20) <= 'z')
            {
                m_strong_direction = -1;
                break;
            }
            i++;
            continue;
        }
        size_t   n  = std::min<size_t>((c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4, m_text.size() - i);
        uint32_t cp = n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
        for(size_t k = 1; k < n; k++)
            cp = (cp << 6) | (static_cast<unsigned char>(m_text[i + k]) & 0x3F);
        i += n;
        // Hebrew, Arabic, Syriac, Thaana, NKo, Samaritan and their presentation forms
        if((cp >= 0x0590 && cp <= 0x08FF) || (cp >= 0xFB1D && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF) ||
           (cp >= 0x10800 && cp <= 0x10FFF) || (cp >= 0x1E800 && cp <= 0x1EFFF))
        {
            m_strong_direction = 1;
            break;
        }
        // Not punctuation, symbols, combining marks, variation selectors or emoji: a letter of a left to right script.
        if(cp >= 0xC0 && cp != 0xD7 && cp != 0xF7 && !(cp >= 0x0300 && cp <= 0x036F) && !(cp >= 0x2000 && cp <= 0x2BFF) &&
           !(cp >= 0x3000 && cp <= 0x303F) && !(cp >= 0xFE00 && cp <= 0xFE6F) && !(cp >= 0xFF00 && cp <= 0xFF20) && cp < 0x1F000)
        {
            m_strong_direction = -1;
            break;
        }
    }
    return m_strong_direction;
}

void litehtml::el_text::set_text(const std::string& text)
{
    m_text = text;
    m_run.reset();
    m_strong_direction = 2;
    m_transformed_text.clear();
    m_use_transformed = false;
    m_draw_spaces     = true;
    m_size            = size();
    m_measured_font   = 0;
    m_breaker.reset();
    mark_layout_dirty();
    m_css = unstyled_text_css();
}

void litehtml::el_text::compute_styles(bool /*recursive*/)
{
    mark_layout_dirty();
    element::ptr el_parent = parent();
    // Every text child of an element has the same style: the parent's text_css(), one object for all of them.
    if(el_parent)
        m_css = el_parent->text_css();
    else
        css_w().set_float(float_none);

    if(m_css->get_text_transform() != text_transform_none)
    {
        m_transformed_text = m_text;
        m_use_transformed  = true;
        get_document()->container()->transform_text(m_transformed_text, m_css->get_text_transform());
    } else
    {
        m_use_transformed = false;
    }

    if(is_white_space())
    {
        m_transformed_text = " ";
        m_use_transformed  = true;
    } else
    {
        if(m_text == "\t")
        {
            m_transformed_text = "    ";
            m_use_transformed  = true;
        }
        if(m_text == "\n" || m_text == "\r")
        {
            m_transformed_text = "";
            m_use_transformed  = true;
        }
    }

    font_metrics fm;
    uint_ptr     font = 0;
    if(el_parent)
    {
        font = el_parent->css().get_font();
        fm   = el_parent->css().get_font_metrics();
    }
    if(is_break() || !font)
    {
        m_size.height      = 0;
        m_size.width       = 0;
        m_measured_font    = 0;
    } else
    {
        m_size.height = fm.height;
        const int transform = m_use_transformed ? (is_white_space() ? -2 : int(m_css->get_text_transform())) : -3;
        if(m_measured_font != font || m_measured_transform != transform)
        {
            m_breaker.reset();
            m_size.width = -1_px;
            m_end_extra  = 0_px;
            if(m_run && !m_use_transformed && !el_parent->css().is_vertical_rl() && m_run_end <= m_run->text.size())
            {
                // The piece's share of its shaped run (font kerning with its neighbours, as browsers break a shaped
                // text node). Not for vertical text: the backend shapes it horizontally, and a vertical run (shaped top
                // to bottom, as Blink does) has no pair adjustments to take from a horizontal shaping.
                const std::vector<pixel_t>* prefix = nullptr;
                for(const auto& entry : m_run->advances)
                    if(entry.first == font)
                        prefix = &entry.second;
                if(!prefix)
                {
                    m_run->advances.emplace_back(font, std::vector<pixel_t>());
                    if(!get_document()->container()->text_advances(m_run->text, font, m_run->advances.back().second))
                        m_run->advances.back().second.clear();
                    prefix = &m_run->advances.back().second;
                }
                // A piece cut inside a cluster (a ligature across a break opportunity: ―― in a CJK font, ff across
                // a soft hyphen) has no share of the run: it is drawn alone, and measured so.
                if(prefix->size() == m_run->text.size() + 1 && !std::isnan(float((*prefix)[m_run_begin])) &&
                   !std::isnan(float((*prefix)[m_run_end])))
                {
                    m_size.width = (*prefix)[m_run_end] - (*prefix)[m_run_begin];
                    m_end_extra  = get_document()->container()->text_width(m_text.c_str(), font) - m_size.width;
                }
            }
            if(m_size.width < 0_px)
                m_size.width = get_document()->container()->text_width(
                    m_use_transformed ? m_transformed_text.c_str() : m_text.c_str(), font);
            if(ends_with_soft_hyphen() && !el_parent->css().is_vertical_rl())
                m_end_extra = get_document()->container()->text_width(hyphenated().c_str(), font) - m_size.width;
            if(el_parent->css().is_vertical_rl() && el_parent->css().combines_upright() && !is_white_space())
            {
                // 縦中横: the run sits upright in one em of the column.
                m_size.width = fm.font_size;
            } else if(el_parent->css().is_vertical_rl() && el_parent->css().get_text_orientation() == text_orientation_upright)
            {
                // Upright text in a vertical line: each character takes an em, as the font's vertical advance.
                m_size.width = fm.font_size * pixel_t(float(utf8_length(display_text())));
            }
            m_measured_font      = font;
            m_measured_transform = transform;
        }
    }
    m_draw_spaces = fm.draw_spaces;
}

std::shared_ptr<litehtml::text_breaker> litehtml::el_text::breaker()
{
    if(!m_breaker)
        m_breaker = doc_ptr()->container()->create_text_breaker(display_text(), css().get_font());
    return m_breaker;
}

void litehtml::el_text::draw(uint_ptr hdc, pixel_t x, pixel_t y, const position* clip,
                             const std::shared_ptr<render_item>& ri)
{
    if(is_white_space() && !m_draw_spaces)
    {
        return;
    }

    const auto text_item = std::dynamic_pointer_cast<render_text>(ri);
    if(text_item && !text_item->fragments.empty())
    {
        for(const auto& fragment : text_item->fragments)
            draw(hdc, x, y, clip, fragment);
        return;
    }

    position pos  = ri->pos();
    pos.x        += x;
    pos.y        += y;
    {
        // The line's top on a device pixel, as Chromium paints text (twidget: the font's device_pixel; boxes keep
        // pixel_snap_scale()).
        const float px = float(css().get_font_metrics().device_pixel);
        const float k  = px > 0 && px < 1 ? 1.0f / px : pixel_snap_scale();
        pos.y          = std::round(pos.y.value() * k) / k;
    }

    position ink = pos;
    {
        pixel_t slack = std::max(pixel_t(4), ink.height / 2_px);
        if(auto owner = parent())
        {
            float reach = 0;
            for(const auto& shadow : owner->css().get_text_shadows())
                reach = std::max(reach, std::abs(shadow.blur.val()) * 2 + std::abs(shadow.spread.val()) +
                                           std::max(std::abs(shadow.x.val()), std::abs(shadow.y.val())));
            slack += pixel_t(reach);
        }
        ink.x     -= slack;
        ink.y     -= slack;
        ink.width += slack * 2_px;
        ink.height += slack * 2_px;
    }
    if(ink.does_intersect(clip))
    {
        element::ptr el_parent = parent();
        if(el_parent)
        {
            document* doc = doc_ptr();

            uint_ptr font = el_parent->css().get_font();
            if(font)
            {
                web_color color = el_parent->css().get_color();
                const std::string& shown = text_item && text_item->is_fragment ? text_item->fragment_text : display_text();
                if(text_item && text_item->cut >= 0)
                {
                    // text-overflow: ellipsis cut it (render_item_inline_context::apply_ellipsis).
                    if(text_item->cut > 0)
                        doc->container()->draw_text(hdc, shown.substr(0, text_item->cut).c_str(), font, color, pos);
                } else if(doc->vertical() && el_parent->css().is_vertical_rl())
                {
                    draw_vertical(doc, hdc, shown, el_parent->css(), color, pos);
                    draw_emphasis(doc, hdc, shown, el_parent->css(), pos, true);
                } else if(text_item && text_item->hyphen)
                {
                    doc->container()->draw_text(hdc, hyphenated().c_str(), font, color, pos);
                } else
                {
                    doc->container()->draw_text(hdc, shown.c_str(), font, color, pos);
                    draw_emphasis(doc, hdc, shown, el_parent->css(), pos, false);
                }
            }
        }
    }
}

std::string litehtml::el_text::dump_get_name()
{
    return "text: \"" + get_escaped_string(m_text) + "\"";
}

std::vector<std::tuple<std::string, std::string>> litehtml::el_text::dump_get_attrs()
{
    return {};
}
