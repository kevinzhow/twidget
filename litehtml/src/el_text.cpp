#include "html.h"
#include "el_text.h"
#include "render_item.h"
#include "render_inline.h"
#include "document_container.h"
#include "vertical.h"
#include "cjk_rules.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

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

    // Where text is drawn in parts: at each gap justify widens (render_text::justify_gap), and around each character
    // punctuation compression trims (render_text::trims), so that it is drawn alone.
    std::vector<size_t> cuts(const std::string& text, litehtml::pixel_t gap, const std::vector<litehtml::char_trim>& trims)
    {
        using namespace litehtml;
        std::vector<size_t> out = gap != 0_px ? cjk::justifiable_gaps(text) : std::vector<size_t>();
        for(const auto& t : trims)
        {
            const unsigned char c0 = text[t.at];
            out.push_back(t.at);
            out.push_back(t.at + (c0 < 0x80 ? 1 : (c0 >> 5) == 6 ? 2 : (c0 >> 4) == 14 ? 3 : 4));
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        out.erase(std::remove_if(out.begin(), out.end(), [&](size_t at) { return at == 0 || at >= text.size(); }), out.end());
        return out;
    }

    // The trim of the character at byte `at` (render_text::trims), or none.
    const litehtml::char_trim* trim_at(const std::vector<litehtml::char_trim>& trims, size_t at)
    {
        for(const auto& t : trims)
            if(t.at == at)
                return &t;
        return nullptr;
    }

    // Horizontal text from pos, gap further on at each justifiable gap (render_text::justify_gap) and nearer by what
    // punctuation compression trims (render_text::trims); a trimmed mark drawn alone, with the font's half-width form
    // (`half`) when it has one, else full width moved back by the half it gives up.
    void draw_justified(const litehtml::el_text& el, litehtml::uint_ptr hdc, const std::string& text,
                        litehtml::uint_ptr font, litehtml::uint_ptr half, litehtml::web_color color,
                        const litehtml::position& pos, litehtml::pixel_t gap, const std::vector<litehtml::char_trim>& trims)
    {
        using namespace litehtml;
        auto* container = el.get_document()->container();
        const std::vector<size_t> parts = cuts(text, gap, trims);
        if(parts.empty() && trims.empty())
        {
            container->draw_text(hdc, text.c_str(), font, color, pos);
            return;
        }
        for(size_t k = 0, from = 0; from < text.size(); k++)
        {
            const size_t      to   = k < parts.size() ? parts[k] : text.size();
            const std::string part = text.substr(from, to - from);
            position          at   = pos;
            uint_ptr          f    = font;
            at.x += el.advance_to(text, from) + justify_shift(text, from, gap) - trim_shift(trims, from);
            if(const char_trim* t = trim_at(trims, from))
            {
                if(container->has_half_width_form(part.c_str(), font, false))
                    f = half;
                else
                    at.x -= t->start;
            }
            at.width = container->text_width(part.c_str(), f);
            container->draw_text(hdc, part.c_str(), f, color, at);
            from = to;
        }
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

    litehtml::pixel_t vertical_advance(litehtml::document_container* container, const std::string& text, size_t end,
                                       litehtml::text_orientation orientation, litehtml::uint_ptr font,
                                       litehtml::uint_ptr upright, litehtml::pixel_t em,
                                       const std::vector<litehtml::cjk::span>& tcy);

    // Draws a mark over (or under) each character of the run, centred on it along the line; in vertical text the
    // marks go on the right (over) and stand upright, one for a tate-chu-yoko token. Line boxes do not grow for them
    // (they use the leading).
    void draw_emphasis(litehtml::document* doc, litehtml::uint_ptr hdc, const std::string& text,
                       const litehtml::css_properties& css, const litehtml::position& pos, bool vertical,
                       litehtml::pixel_t gap, const std::vector<litehtml::char_trim>& trims,
                       const std::vector<litehtml::cjk::span>& tcy)
    {
        using namespace litehtml;
        const uint_ptr mark_font = css.get_font_emphasis();
        const std::string mark   = emphasis_mark(css.get_text_emphasis_style(), vertical);
        if(!mark_font || mark.empty())
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
            auto token = std::find_if(tcy.begin(), tcy.end(), [&](const cjk::span& t) { return t.from <= i && i < t.to; });
            if(!emphasis_skips(cp) && (token == tcy.end() || token->from == i))
            {
                pixel_t x0, w;
                if(vertical)
                {
                    x0 = vertical_advance(container, text, i, css.get_text_orientation(), css.get_font(),
                                          css.get_font_upright(), fm.font_size, tcy);
                    w  = em_each || token != tcy.end()
                             ? fm.font_size
                             : vertical_advance(container, text, i + n, css.get_text_orientation(), css.get_font(),
                                                css.get_font_upright(), fm.font_size, tcy) - x0;
                } else
                {
                    x0 = i ? container->text_width(text.substr(0, i).c_str(), css.get_font()) : 0_px;
                    w  = container->text_width(text.substr(i, n).c_str(), css.get_font());
                }
                x0 += justify_shift(text, i, gap) - trim_shift(trims, i);
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

    // Whether a character of vertical text is drawn upright: text-orientation upright, or mixed and upright by UAX #50
    // (a Tr character only when its font has a vertical form).
    bool drawn_upright(litehtml::document_container* container, litehtml::text_orientation orientation, char32_t cp,
                       const std::string& glyph, litehtml::uint_ptr font)
    {
        using namespace litehtml;
        return orientation == text_orientation_upright ||
               (orientation == text_orientation_mixed && upright_in_vertical(cp) &&
                (!turned_in_vertical(cp) || container->has_vertical_form(glyph.c_str(), font)));
    }

    // The code point at text[i], and its length in bytes.
    char32_t code_point_at(const std::string& text, size_t i, size_t& n)
    {
        const unsigned char c0 = text[i];
        n = c0 < 0x80 ? 1 : (c0 >> 5) == 6 ? 2 : (c0 >> 4) == 14 ? 3 : 4;
        n = std::min(n, text.size() - i);
        char32_t cp = n == 1 ? c0 : n == 2 ? c0 & 0x1F : n == 3 ? c0 & 0x0F : c0 & 0x07;
        for(size_t k = 1; k < n; k++)
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        return cp;
    }

    // The advance of text[0, end) along a vertical line: a tate-chu-yoko token an em (a boundary inside one, its share
    // of the em by characters); with text-orientation: upright, each other character an em; else each upright character
    // measured alone in the font it is drawn with (`upright`: vert, its vertical form, maybe from another face than the
    // font's: twidget::text's CJK face for a Latin font's …), as Blink shapes vertical text top to bottom with no pair
    // kerning between them, and each run of sideways characters as one horizontal run.
    litehtml::pixel_t vertical_advance(litehtml::document_container* container, const std::string& text, size_t end,
                                       litehtml::text_orientation orientation, litehtml::uint_ptr font,
                                       litehtml::uint_ptr upright, litehtml::pixel_t em,
                                       const std::vector<litehtml::cjk::span>& tcy)
    {
        using namespace litehtml;
        pixel_t advance = 0_px;
        size_t  run     = 0;  // start of the sideways run not measured yet
        auto    sideways = [&](size_t to) {
            if(to > run)
                advance += container->text_width(text.substr(run, to - run).c_str(), font);
        };
        auto token = tcy.begin();
        for(size_t i = 0, n = 0; i < end; i += n)
        {
            if(token != tcy.end() && token->from == i)
            {
                sideways(i);
                if(end < token->to)
                    return advance + em * pixel_t(float(utf8_length(text.substr(i, end - i)))) /
                                         pixel_t(float(utf8_length(text.substr(i, token->to - i))));
                advance += em;
                n        = token->to - i;
                run      = token->to;
                ++token;
                continue;
            }
            const char32_t    cp    = code_point_at(text, i, n);
            const std::string glyph = text.substr(i, n);
            if(orientation != text_orientation_upright && !drawn_upright(container, orientation, cp, glyph, font))
                continue;
            sideways(i);
            advance += orientation == text_orientation_upright ? em : container->text_width(glyph.c_str(), upright ? upright : font);
            run = i + n;
        }
        sideways(end);
        return advance;
    }

    // Vertical text (document::vertical()): the page's turn already makes a run sideways, so runs of sideways
    // characters are drawn as horizontal text is; each upright character is turned back about its em box's centre
    // and drawn with the font's vertical forms, and so is each tate-chu-yoko token, its characters side by side. The em
    // box's centre is the line's central baseline.
    void draw_vertical(litehtml::document* doc, litehtml::uint_ptr hdc, const std::string& text,
                       const litehtml::css_properties& css, litehtml::web_color color, const litehtml::position& pos,
                       litehtml::pixel_t gap, const std::vector<litehtml::char_trim>& trims,
                       const std::vector<litehtml::cjk::span>& tcy)
    {
        using namespace litehtml;
        auto*               container   = doc->container();
        const uint_ptr      font        = css.get_font();
        const uint_ptr      upright     = css.get_font_upright();
        const font_metrics& fm          = css.get_font_metrics();
        const pixel_t       em          = fm.font_size;
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
        // text-orientation: upright gives each character an em; mixed keeps the advances.
        const bool all_upright = orientation == text_orientation_upright;
        // Justified, the characters are that much further apart at their gaps (render_text::justify_gap); compressed,
        // nearer by what the marks before them give up (render_text::trims).
        const std::vector<size_t> parts = cuts(text, gap, trims);
        auto advance = [&](size_t end) {
            return vertical_advance(container, text, end, orientation, font, upright, em, tcy) + justify_shift(text, end, gap) -
                   trim_shift(trims, end);
        };
        // A sideways run is drawn a piece between two gaps (or around a trimmed mark: full width, moved back by the
        // half it gives up) at a time.
        auto flush = [&](size_t end) {
            for(size_t from = run; from < end;)
            {
                auto          next = std::upper_bound(parts.begin(), parts.end(), from);
                const size_t  to   = next != parts.end() && *next < end ? *next : end;
                position      at   = pos;
                at.x              += advance(from);
                if(const char_trim* t = trim_at(trims, from))
                    at.x -= t->start;
                at.width           = container->text_width(text.substr(from, to - from).c_str(), font);
                container->draw_text(hdc, text.substr(from, to - from).c_str(), font, color, at);
                from = to;
            }
        };
        auto token = tcy.begin();
        for(size_t i = 0, n = 0; i < text.size(); i += n)
        {
            if(token != tcy.end() && token->from == i)
            {
                // 縦中横: the token drawn as horizontal text, centred in its em of the column, squeezed across it when
                // wider (as Blink fits text-combine-upright; stone lets two digits overhang the em).
                flush(i);
                const std::string glyphs = text.substr(i, token->to - i);
                const pixel_t     w      = container->text_width(glyphs.c_str(), font);
                const pixel_t     cx     = pos.x + advance(i) + em / 2_px;
                const float       s      = w > em ? float(em / w) : 1.0f;
                paint_group       group;
                group.matrix = {0, -s, 1, 0, float(cx - cy), s * float(cx) + float(cy)};
                group.bounds = position(cx - w / 2_px, pos.y, w, pos.height);
                container->begin_group(hdc, group);
                container->draw_text(hdc, glyphs.c_str(), font, color, group.bounds);
                container->end_group(hdc);
                n   = token->to - i;
                run = token->to;
                ++token;
                continue;
            }
            const char32_t    cp    = code_point_at(text, i, n);
            const std::string glyph = text.substr(i, n);
            if(!drawn_upright(container, orientation, cp, glyph, font))
                continue;
            flush(i);
            // A trimmed mark: the font's half-width form (vhal) where its em box starts, else its full-width
            // form moved back by the half it gives up.
            uint_ptr          f     = upright;
            pixel_t           back  = 0_px;
            if(const char_trim* t = trim_at(trims, i))
            {
                if(container->has_half_width_form(glyph.c_str(), font, true))
                    f = css.get_font_half();
                else
                    back = t->start;
            }
            const pixel_t     w     = container->text_width(glyph.c_str(), upright);
            const pixel_t     cx    = pos.x + advance(i) - back + (all_upright ? em : w) / 2_px;
            paint_group       group;
            group.matrix = upright_matrix(float(cx), float(cy));
            group.bounds = position(cx - w / 2_px, pos.y, w, pos.height);
            container->begin_group(hdc, group);
            container->draw_text(hdc, glyph.c_str(), f, color, position(cx - w / 2_px, pos.y, w, pos.height));
            container->end_group(hdc);
            run = i + n;
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
    m_tate_chu_yoko.clear();
    m_strong_direction = 2;
    m_transformed_text.clear();
    m_use_transformed = false;
    m_draw_spaces     = true;
    m_size            = size();
    m_measured_font   = 0;
    m_breaker.reset();
    m_hyphenation.reset();
    mark_layout_dirty();
    m_css = unstyled_text_css();
}

void litehtml::el_text::compute_styles(bool /*recursive*/)
{
    mark_layout_dirty();
    m_hyphenation.reset();
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

    // Tate-chu-yoko: the tokens of its text set upright in one em, found in its text node's whole text, where the
    // characters around it decide where a token ends (1 before ２ is not one).
    std::vector<cjk::span> tcy;
    const std::string&     shown = display_text();
    if(el_parent && el_parent->css().is_vertical_rl() && el_parent->css().get_text_orientation() != text_orientation_sideways &&
       !is_white_space() && !is_break() && !shown.empty())
    {
        if(el_parent->css().combines_upright())
            tcy.push_back({0, shown.size()});
        else if(m_run && !m_use_transformed && m_run_end <= m_run->text.size())
            for(const auto& t : cjk::tate_chu_yoko(m_run->text, m_run_begin, m_run_end))
                tcy.push_back({t.from - m_run_begin, t.to - m_run_begin});
        else
            tcy = cjk::tate_chu_yoko(shown, 0, shown.size());
    }
    if(tcy != m_tate_chu_yoko)
    {
        m_tate_chu_yoko = std::move(tcy);
        m_measured_font = 0;
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
            {
                // Vertical text as draw_vertical places it: its upright characters not kerned, its tate-chu-yoko
                // tokens an em each.
                const std::string& measured = m_use_transformed ? m_transformed_text : m_text;
                m_size.width = el_parent->css().is_vertical_rl()
                                   ? vertical_advance(get_document()->container(), measured, measured.size(),
                                                      el_parent->css().get_text_orientation(), font,
                                                      el_parent->css().get_font_upright(), fm.font_size, m_tate_chu_yoko)
                                   : get_document()->container()->text_width(measured.c_str(), font);
            }
            if(ends_with_soft_hyphen() && !el_parent->css().is_vertical_rl())
                m_end_extra = get_document()->container()->text_width(hyphenated(display_text()).c_str(), font) - m_size.width;
            m_measured_font      = font;
            m_measured_transform = transform;
        }
    }
    m_draw_spaces = fm.draw_spaces;
}

const std::vector<size_t>& litehtml::el_text::hyphenation_points()
{
    if(m_hyphenation)
        return *m_hyphenation;
    m_hyphenation.emplace();
    element::ptr el_parent = parent();
    if(!el_parent || is_white_space() || is_break() || el_parent->css().is_vertical_rl())
        return *m_hyphenation;
    const css_properties& css   = el_parent->css();
    document_container*   cont  = get_document()->container();
    const bool            on    = css.get_hyphens() == hyphens_auto ||
                                  (css.get_hyphens() == hyphens_initial && css.get_text_align() == text_align_justify &&
                                   cont->hyphenate_justified());
    const std::string&    shown = display_text();
    if(!on || shown.find("\xC2\xAD") != std::string::npos)
        return *m_hyphenation;
    // A whole word only: after a space, an element's start or a dash; not the rest of a word after a soft hyphen, nor
    // the part of a URL after its / (the piece before this one ends there).
    const auto& siblings = el_parent->children();
    auto        self     = std::find_if(siblings.begin(), siblings.end(), [this](const element::ptr& e) { return e.get() == this; });
    if(self != siblings.begin() && self != siblings.end())
        if(auto before = std::dynamic_pointer_cast<el_text>(*std::prev(self)); before && !before->is_white_space())
        {
            const std::string& prev = before->display_text();
            const bool dash = (!prev.empty() && prev.back() == '-') ||
                              (prev.size() >= 3 && prev.compare(prev.size() - 3, 2, "\xE2\x80") == 0 &&
                               (prev.back() == '\x90' || prev.back() == '\x93' || prev.back() == '\x94'));
            if(!dash)
                return *m_hyphenation;
        }
    // The language: the nearest lang or xml:lang (an empty one says it is unknown).
    std::string language;
    for(element::ptr el = el_parent; el; el = el->parent())
    {
        const char* lang = el->get_attr("lang");
        if(!lang)
            lang = el->get_attr("xml:lang");
        if(lang)
        {
            language = lang;
            break;
        }
    }
    cont->hyphenate(shown, language, *m_hyphenation);
    return *m_hyphenation;
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
                const pixel_t      gap   = text_item ? text_item->justify_gap : 0_px;
                static const std::vector<char_trim> none;
                const auto&        trims = text_item && !text_item->is_fragment ? text_item->trims : none;
                if(text_item && text_item->cut >= 0)
                {
                    // text-overflow: ellipsis cut it (render_item_inline_context::apply_ellipsis).
                    if(text_item->cut > 0)
                        doc->container()->draw_text(hdc, shown.substr(0, text_item->cut).c_str(), font, color, pos);
                } else if(doc->vertical() && el_parent->css().is_vertical_rl())
                {
                    static const std::vector<cjk::span> no_tokens;
                    const auto& tcy = text_item && text_item->is_fragment ? no_tokens : m_tate_chu_yoko;
                    draw_vertical(doc, hdc, shown, el_parent->css(), color, pos, gap, trims, tcy);
                    draw_emphasis(doc, hdc, shown, el_parent->css(), pos, true, gap, trims, tcy);
                } else if(text_item && text_item->hyphen)
                {
                    draw_justified(*this, hdc, hyphenated(shown), font, el_parent->css().get_font_half(), color, pos, gap, trims);
                } else
                {
                    draw_justified(*this, hdc, shown, font, el_parent->css().get_font_half(), color, pos, gap, trims);
                    draw_emphasis(doc, hdc, shown, el_parent->css(), pos, false, gap, trims, {});
                }
            }
        }
    }
}

litehtml::pixel_t litehtml::el_text::advance_to(const std::string& shown, size_t bytes) const
{
    if(bytes == 0)
        return 0_px;
    element::ptr   el_parent = parent();
    const uint_ptr font      = el_parent ? el_parent->css().get_font() : 0;
    if(el_parent && el_parent->css().is_vertical_rl())
    {
        static const std::vector<cjk::span> no_tokens;
        const css_properties& css = el_parent->css();
        return vertical_advance(get_document()->container(), shown, bytes, css.get_text_orientation(), font,
                                css.get_font_upright(), css.get_font_metrics().font_size, shown == display_text() ? m_tate_chu_yoko : no_tokens);
    }
    if(!m_use_transformed && m_run && bytes <= shown.size() && m_run_end - m_run_begin == shown.size() && shown == m_text)
        for(const auto& entry : m_run->advances)
            if(entry.first == font && entry.second.size() == m_run->text.size() + 1)
            {
                const pixel_t from = entry.second[m_run_begin], to = entry.second[m_run_begin + bytes];
                if(!std::isnan(float(from)) && !std::isnan(float(to)))
                    return to - from;
            }
    return get_document()->container()->text_width(shown.substr(0, bytes).c_str(), font);
}

litehtml::pixel_t litehtml::justify_shift(const std::string& text, size_t at, pixel_t gap, bool after_gap)
{
    if(gap == 0_px)
        return 0_px;
    const std::vector<size_t> gaps = cjk::justifiable_gaps(text);
    const auto end = after_gap ? std::upper_bound(gaps.begin(), gaps.end(), at) : std::lower_bound(gaps.begin(), gaps.end(), at);
    return gap * pixel_t(float(end - gaps.begin()));
}

litehtml::pixel_t litehtml::trim_shift(const std::vector<char_trim>& trims, size_t at)
{
    pixel_t shift = 0_px;
    for(const auto& t : trims)
        if(t.at < at)
            shift += t.start + t.end;
    return shift;
}

std::vector<litehtml::char_trim> litehtml::el_text::compression(const std::string& shown, char32_t before, char32_t after,
                                                                uint8_t first, uint8_t last) const
{
    std::vector<char_trim> out;
    element::ptr           el_parent = parent();
    if(!el_parent || shown.empty())
        return out;
    const css_properties& css = el_parent->css();
    std::vector<std::pair<size_t, char32_t>> chars;
    for(size_t i = 0, n = 0; i < shown.size(); i += n)
        chars.push_back({i, code_point_at(shown, i, n)});
    for(size_t k = 0; k < chars.size(); k++)
    {
        const char32_t prev = k ? chars[k - 1].second : before;
        const char32_t next = k + 1 < chars.size() ? chars[k + 1].second : after;
        const uint8_t  at   = (k == 0 ? first : 0) | (k + 1 == chars.size() ? last : 0);
        // A tate-chu-yoko token's characters are set together in their em.
        if(std::any_of(m_tate_chu_yoko.begin(), m_tate_chu_yoko.end(),
                       [&](const cjk::span& t) { return t.from <= chars[k].first && chars[k].first < t.to; }))
            continue;
        const cjk::trim t   = cjk::compression(prev, chars[k].second, next, at);
        if(t.start == 0 && t.end == 0)
            continue;
        const size_t  from    = chars[k].first;
        const size_t  to      = k + 1 < chars.size() ? chars[k + 1].first : shown.size();
        const pixel_t advance = css.is_vertical_rl() && css.get_text_orientation() == text_orientation_upright
                                    ? css.get_font_metrics().font_size
                                    : get_document()->container()->text_width(shown.substr(from, to - from).c_str(), css.get_font());
        out.push_back({from, advance * pixel_t(t.start), advance * pixel_t(t.end)});
    }
    return out;
}

void litehtml::render_text::compress(uint8_t first, uint8_t last)
{
    const pixel_t was  = trim_shift(trims, SIZE_MAX);
    auto          text = std::dynamic_pointer_cast<el_text>(src_el());
    if(text && !is_fragment && !text->is_white_space() && !text->is_break())
        trims = text->compression(text->display_text(), before, after, first, last);
    else
        trims.clear();
    pos().width += was - trim_shift(trims, SIZE_MAX);
}

std::string litehtml::el_text::dump_get_name()
{
    return "text: \"" + get_escaped_string(m_text) + "\"";
}

std::vector<std::tuple<std::string, std::string>> litehtml::el_text::dump_get_attrs()
{
    return {};
}
