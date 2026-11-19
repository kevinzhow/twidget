#include "render_inline_context.h"
#include "render_ruby.h"
#include "document.h"
#include "iterators.h"
#include "types.h"
#include "el_text.h"
#include "html_tag.h"
#include "document_container.h"
#include "cjk_rules.h"
#include <algorithm>
#include <functional>

namespace
{
    class wrapped_text_item : public litehtml::line_box_item
    {
      public:
        wrapped_text_item(const std::shared_ptr<litehtml::render_item>& item, litehtml::pixel_t minimum) : line_box_item(item)
        {
            set_rendered_min_width(minimum);
        }
        litehtml::pixel_t get_rendered_natural_width() const override { return width(); }
    };
}

litehtml::rendered_width litehtml::render_item_inline_context::measure(pixel_t x, pixel_t y,
    const containing_block_context& cb, formatting_context* fmt_ctx, bool second_pass, pixel_t* measured_height)
{
    if(!measured_height && (cb.size_mode & containing_block_context::size_mode_content) &&
       css().get_width().is_predefined() && css().get_min_width().is_predefined() && css().get_max_width().is_predefined() &&
       !css().contains(contain_size) && !css().contains(contain_inline_size) &&
       (css().get_list_style_type() == list_style_type_none || css().get_list_style_position() != list_style_position_inside) &&
       css().get_text_indent().val() == 0 &&
       !m_children.empty())
    {
        calc_outlines(cb.width);
        auto self_size = calculate_containing_block_context(cb);
        if(static_cast<pixel_t>(self_size.render_width) <= 0_px)
        {
            pixel_t minimum = 0_px;
            bool supported = true;
            for(const auto& child : m_children)
            {
                auto text = std::dynamic_pointer_cast<el_text>(child->src_el());
                if(!text || text->css().get_wrapping() != overflow_wrap_anywhere ||
                   is_one_of(text->css().get_white_space(), white_space_pre, white_space_nowrap))
                {
                    supported = false;
                    break;
                }
                if(text->is_white_space())
                    continue;
                auto breaker = text->breaker();
                if(!breaker)
                {
                    supported = false;
                    break;
                }
                minimum = std::max(minimum, breaker->minimum_width());
            }
            if(supported)
            {
                rendered_width result;
                result.natural_width = minimum + content_offset_width();
                result.min_width = is_one_of(css().get_overflow(), overflow_visible, overflow_clip) ?
                    result.natural_width : content_offset_width();
                return result;
            }
        }
    }
    return render_item::measure(x, y, cb, fmt_ctx, second_pass, measured_height);
}

litehtml::size litehtml::render_item_inline_context::text_content_size() const
{
    pixel_t width = m_rendered_width.natural_width;
    pixel_t height = m_line_boxes.empty() ? 0_px : m_line_boxes.back()->bottom();
    if(src_el()->tag() == _textarea_)
    {
        const pixel_t line = css().line_height().computed_value;
        height = pixel_t(static_cast<float>(m_line_boxes.size())) * line;
        if(!m_children.empty() && m_children.back()->src_el()->is_break())
            height += line;
        height = std::max(height, line);
        if(css().get_white_space() == white_space_pre_wrap)
        {
            width = 0_px;
            for(const auto& child : m_children)
                if(!child->src_el()->is_space())
                    width = std::max(width, child->right());
        }
    }
    return size(width, height);
}


namespace
{
    char32_t decode_at(const std::string& s, size_t i)
    {
        const unsigned char c0 = s[i];
        size_t n = c0 < 0x80 ? 1 : (c0 >> 5) == 6 ? 2 : (c0 >> 4) == 14 ? 3 : 4;
        n        = std::min(n, s.size() - i);
        char32_t cp = n == 1 ? c0 : n == 2 ? c0 & 0x1F : n == 3 ? c0 & 0x0F : c0 & 0x07;
        for(size_t k = 1; k < n; k++)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        return cp;
    }
    char32_t first_char(const std::string& s) { return s.empty() ? 0 : decode_at(s, 0); }
    char32_t last_char(const std::string& s)
    {
        if(s.empty())
            return 0;
        size_t i = s.size() - 1;
        while(i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80)
            i--;
        return decode_at(s, i);
    }

    // East_Asian_Width F, W or H, Hangul left out (CSS Text 3 §4.1.3), by blocks.
    bool east_asian_wide(char32_t c)
    {
        if((c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xFFA0 && c <= 0xFFDC))
            return false;
        return (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) || (c >= 0x3400 && c <= 0x4DBF) ||
               (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) || (c >= 0xF900 && c <= 0xFAFF) ||
               (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFFE6) || (c >= 0x20000 && c <= 0x3FFFD);
    }

    // The first (or last) character of an inline item's text: a text's own; a ruby's base text (its readings are
    // beside the line); none for anything else.
    char32_t edge_char(const litehtml::element::ptr& el, bool first)
    {
        if(!el)
            return 0;
        if(el->is_text())
        {
            auto text = std::dynamic_pointer_cast<litehtml::el_text>(el);
            return text ? (first ? first_char(text->display_text()) : last_char(text->display_text())) : 0;
        }
        // A base unit of a word of ruby pairs (an anonymous box of its <ruby>): its base's text, from its render items.
        if(auto pair = std::dynamic_pointer_cast<litehtml::render_item_ruby_pair>(el->get_render_item());
           pair && pair->in_word() && !pair->children().empty())
        {
            std::function<char32_t(const std::shared_ptr<litehtml::render_item>&)> walk =
                [&](const std::shared_ptr<litehtml::render_item>& ri) -> char32_t {
                if(auto text = std::dynamic_pointer_cast<litehtml::el_text>(ri->src_el()); text && !text->is_white_space())
                    return first ? first_char(text->display_text()) : last_char(text->display_text());
                const auto& kids = ri->children();
                if(first)
                {
                    for(auto it = kids.begin(); it != kids.end(); ++it)
                        if(char32_t c = walk(*it))
                            return c;
                } else
                {
                    for(auto it = kids.rbegin(); it != kids.rend(); ++it)
                        if(char32_t c = walk(*it))
                            return c;
                }
                return 0;
            };
            return walk(pair->children().back());
        }
        if(el->tag() != litehtml::_ruby_ && !(el->parent() && el->parent()->tag() == litehtml::_ruby_))
            return 0;
        const auto& kids = el->children();
        auto pick = [&](const litehtml::element::ptr& child) -> char32_t {
            if(child->tag() == litehtml::_rt_ || child->tag() == litehtml::_rp_ || child->is_white_space())
                return 0;
            if(child->is_text())
                return edge_char(child, first);
            std::string text;
            child->get_text(text);
            return first ? first_char(text) : last_char(text);
        };
        if(first)
        {
            for(auto it = kids.begin(); it != kids.end(); ++it)
                if(char32_t c = pick(*it))
                    return c;
        } else
        {
            for(auto it = kids.rbegin(); it != kids.rend(); ++it)
                if(char32_t c = pick(*it))
                    return c;
        }
        return 0;
    }

    // A typographic letter unit for word-break: keep-all: a letter or digit (kana, kanji, Hangul, Latin), not
    // punctuation, a symbol or a space.
    bool letter(char32_t c)
    {
        if(c < 0x80)
            return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if((c >= 0x2000 && c <= 0x2BFF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFE30 && c <= 0xFE4F) ||
           (c >= 0xFF00 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) ||
           (c >= 0xFF5B && c <= 0xFF65) || c == 0x30FB)
            return false;
        return litehtml::cjk::classify(c) == 0 || c == 0x30FC;  // ー is part of a word
    }
} // namespace

bool litehtml::same_text(const element::ptr& before, const element::ptr& after)
{
    const auto parent = before->parent();
    if(!parent || parent != after->parent())
        return false;
    const auto& kids = parent->children();
    for(auto it = kids.begin(); it != kids.end(); ++it)
        if(*it == before)
            return std::next(it) != kids.end() && *std::next(it) == after;
    return false;
}

bool litehtml::no_break_opportunity(const element::ptr& before, const element::ptr& after, bool kinsoku)
{
    // The nearest common ancestor: the first of before's ancestors that is one of after's.
    element::ptr common;
    {
        std::vector<const element*> theirs;
        for(auto p = after->parent(); p; p = p->parent())
            theirs.push_back(p.get());
        for(auto p = before->parent(); p && !common; p = p->parent())
            if(std::find(theirs.begin(), theirs.end(), p.get()) != theirs.end())
                common = p;
    }
    if(common && is_one_of(common->css().get_white_space(), white_space_nowrap, white_space_pre))
        return true;
    if(before->is_space() || after->is_space())
        return false;
    const char32_t last = edge_char(before, false), first = edge_char(after, true);
    if(common && common->css().get_word_break() == word_break_keep_all && letter(last) && letter(first))
        return true;
    return kinsoku && cjk::no_break_between(last, first);
}

litehtml::rendered_width litehtml::render_item_inline_context::_render_content(
    pixel_t /*x*/, pixel_t /*y*/, bool /*second_pass*/, const containing_block_context& self_size,
    formatting_context* fmt_ctx)
{
    m_line_boxes.clear();
    m_fragmented_texts.clear();
    m_rendered_width.reset();
    m_va_applied = 0_px;

    white_space ws          = src_el()->css().get_white_space();
    bool        skip_spaces = false;
    if(ws == white_space_normal || ws == white_space_nowrap || ws == white_space_pre_line)
    {
        skip_spaces = true;
    }

    bool was_space = false;
    const bool cjk_breaks = skip_spaces && ws != white_space_pre_line && src_el()->get_document()->cjk_segment_breaks();
    element::ptr before;           // the last element placed (cjk_breaks)
    size_t     removed_to = 0;     // events before this one are a removed segment break

    go_inside_inline  go_inside_inlines_selector;
    inline_selector   select_inlines;
    elements_iterator inlines_iter(true, &go_inside_inlines_selector, &select_inlines);

    std::vector<std::pair<std::shared_ptr<render_item>, iterator_item_type>> events;
    inlines_iter.process(shared_from_this(), [&](const std::shared_ptr<render_item>& el, iterator_item_type item_type) {
        events.emplace_back(el, item_type);
    });
    // Punctuation compression looks at the characters next to each text in the line's flow, across inline elements:
    // a space is one; a run of white space with a segment break that the CJK rule removes is none (the texts meet); a
    // <br>, an inline box (a ruby: its base text) or the block's edge ends the flow.
    {
        std::vector<size_t> kids;
        for(size_t index = 0; index < events.size(); index++)
            if(events[index].second == iterator_item_type_child)
                kids.push_back(index);
        std::vector<char32_t> first(kids.size()), last(kids.size());
        std::vector<bool>     gone(kids.size(), false);
        for(size_t k = 0; k < kids.size(); k++)
        {
            const auto& el = events[kids[k]].first->src_el();
            first[k] = el->is_white_space() ? U' ' : edge_char(el, true);
            last[k]  = el->is_white_space() ? U' ' : edge_char(el, false);
        }
        for(size_t k = 0; k < kids.size(); k++)
        {
            if(!cjk_breaks || !events[kids[k]].first->src_el()->is_white_space() || k == 0 || gone[k])
                continue;
            size_t end     = k;
            bool   segment = false;
            for(; end < kids.size() && events[kids[end]].first->src_el()->is_white_space(); end++)
            {
                std::string t;
                events[kids[end]].first->src_el()->get_text(t);
                segment = segment || t.find('\n') != std::string::npos;
            }
            if(segment && end < kids.size() && east_asian_wide(last[k - 1]) && east_asian_wide(first[end]))
                for(size_t g = k; g < end; g++)
                    gone[g] = true;
        }
        char32_t before = 0;
        for(size_t k = 0; k < kids.size(); k++)
        {
            if(gone[k])
                continue;
            if(auto text = std::dynamic_pointer_cast<render_text>(events[kids[k]].first))
            {
                text->before = before;
                text->after  = 0;
                for(size_t n = k + 1; n < kids.size(); n++)
                    if(!gone[n])
                    {
                        text->after = first[n];
                        break;
                    }
            }
            before = last[k];
        }
    }
    for(size_t index = 0; index < events.size(); index++)
    {
        const auto& el        = events[index].first;
        const auto  item_type = events[index].second;
        m_trailing            = 0_px;
        if(item_type == iterator_item_type_child)
        {
            size_t next = index + 1;
            for(; next < events.size() && events[next].second == iterator_item_type_end_parent; next++)
                m_trailing += lbi_end(events[next].first).width();
            // The next pieces stay on this line with this one while there is no break opportunity between them
            // (no_break_opportunity; a ruby counts by its base text, on either side: <ruby>仕切<rt>しき</rt></ruby>って
            // keeps 切 with っ): their width is kept free after it, so that they go to the next line together. Not past
            // the line's width: what does not fit an empty line overflows it anyway.
            auto is_ruby = [](const std::shared_ptr<render_item>& item) {
                return item->src_el()->tag() == _ruby_ || std::dynamic_pointer_cast<render_item_ruby_pair>(item);
            };
            auto is_piece = [](const std::shared_ptr<render_item>& item) {
                return item->src_el()->is_text() && !item->src_el()->is_break();
            };
            const bool  block_wraps = !is_one_of(ws, white_space_nowrap, white_space_pre);
            std::shared_ptr<render_item> at = el;
            size_t      at_index = index;
            while(block_wraps && (is_piece(at) || is_ruby(at)) && m_trailing < self_size.render_width)
            {
                // Starts of elements, and whole empty ones (<span/>), between the two pieces.
                pixel_t starts = 0_px;
                size_t  n      = next;
                for(; n < events.size() && events[n].second != iterator_item_type_child; n++)
                    starts += events[n].second == iterator_item_type_start_parent ? lbi_start(events[n].first).width()
                                                                                 : lbi_end(events[n].first).width();
                if(n >= events.size() || events[n].second != iterator_item_type_child)
                    break;
                const auto& following = events[n].first;
                const bool  across    = n > at_index + 1 || is_ruby(at) || is_ruby(following);
                bool        joined    = false;
                if(is_piece(at) && is_piece(following))
                    joined = no_break_opportunity(at->src_el(), following->src_el(), across);
                else if((is_piece(following) || is_ruby(following)) && !following->src_el()->is_space() &&
                        !at->src_el()->is_space())
                    joined = cjk::no_break_between(edge_char(at->src_el(), false), edge_char(following->src_el(), true));
                if(!joined)
                    break;
                pixel_t width = 0_px;
                if(is_piece(following))
                {
                    size measured;
                    following->src_el()->get_content_size(measured, self_size.render_width);
                    width = measured.width;
                } else
                {
                    // As place_inline measures an inline box: at its shrink-to-fit width, not the line's (render first: it
                    // sets the width).
                    const pixel_t natural = following->render(0_px, 0_px, self_size, fmt_ctx).natural_width;
                    width = std::min(natural, following->width());
                }
                m_trailing += starts + width;
                for(n++; n < events.size() && events[n].second == iterator_item_type_end_parent; n++)
                    m_trailing += lbi_end(events[n].first).width();
                at       = following;
                at_index = n - 1;
                while(at_index > 0 && events[at_index].second != iterator_item_type_child)
                    at_index--;
                next = n;
            }
        }
        [&]() {
        switch(item_type)
        {
        case iterator_item_type_child:
            {
                if(cjk_breaks && index < removed_to && el->src_el()->is_white_space())
                {
                    el->skip(true);
                    return;
                }
                if(cjk_breaks && el->src_el()->is_white_space() && !was_space && before && east_asian_wide(edge_char(before, false)))
                {
                    // A run of collapsible white space with a segment break in it, between two wide characters:
                    // removed (CSS Text 3 §4.1.3).
                    bool   segment = false;
                    size_t next    = index;
                    for(; next < events.size(); next++)
                    {
                        const auto& e = events[next];
                        if(e.second != iterator_item_type_child)
                            continue;
                        if(!e.first->src_el()->is_white_space())
                            break;
                        std::string t;
                        e.first->src_el()->get_text(t);
                        segment = segment || t.find('\n') != std::string::npos;
                    }
                    if(segment && next < events.size() && east_asian_wide(edge_char(events[next].first->src_el(), true)))
                    {
                        removed_to = next;
                        el->skip(true);
                        return;
                    }
                }
                // skip spaces to make rendering a bit faster
                if(skip_spaces)
                {
                    if(el->src_el()->is_white_space())
                    {
                        if(was_space)
                        {
                            el->skip(true);
                            return;
                        }
                        was_space = true;
                    } else
                    {
                        // skip all spaces after line break
                        was_space = el->src_el()->is_break();
                    }
                }
                if(cjk_breaks && !el->src_el()->is_white_space())
                    before = el->src_el();
                // place element into rendering flow
                place_inline(std::make_unique<line_box_item>(el), self_size, fmt_ctx);
            }
            break;

        case iterator_item_type_start_parent:
            {
                el->clear_inline_boxes();
                el->pos() = position();
                place_inline(std::make_unique<lbi_start>(el), self_size, fmt_ctx);
            }
            break;

        case iterator_item_type_end_parent:
            {
                place_inline(std::make_unique<lbi_end>(el), self_size, fmt_ctx);
            }
            break;
        }
        }();
    }
    m_trailing = 0_px;

    finish_last_box(true, self_size);
    make_room_for_ruby();
    for(const auto& text : m_fragmented_texts)
    {
        text->update_fragment_bounds();
        if(text->css().get_wrapping() != overflow_wrap_anywhere || static_cast<pixel_t>(self_size.render_width) > 0_px)
        {
            size unbroken;
            text->src_el()->get_content_size(unbroken, self_size.render_width);
            m_rendered_width.natural_width = std::max(m_rendered_width.natural_width, unbroken.width);
        }
    }

    apply_ellipsis(self_size.render_width);

    if(!m_line_boxes.empty())
    {
        if(collapse_top_margin())
        {
            pixel_t old_top = m_margins.top;
            m_margins.top   = std::max(m_line_boxes.front()->top_margin(), m_margins.top);
            if(m_margins.top != old_top)
            {
                fmt_ctx->update_floats(m_margins.top - old_top, shared_from_this());
            }
        }
        if(collapse_bottom_margin())
        {
            m_margins.bottom = std::max(m_line_boxes.back()->bottom_margin(), m_margins.bottom);
            m_pos.height     = m_line_boxes.back()->bottom() - m_line_boxes.back()->bottom_margin();
        } else
        {
            m_pos.height = m_line_boxes.back()->bottom();
        }
    }

    return m_rendered_width;
}

void litehtml::render_item_inline_context::space_around(pixel_t width)
{
    if(m_line_boxes.size() != 1)
        return;
    std::vector<line_box_item*> items;
    pixel_t                     left = 0_px, right = 0_px, used = 0_px;
    for(const auto& item : m_line_boxes.front()->items())
    {
        if(item->get_type() != line_box_item::type_text_part || item->get_el()->src_el()->is_white_space())
            continue;
        left  = items.empty() ? item->left() : std::min(left, item->left());
        right = items.empty() ? item->right() : std::max(right, item->right());
        used += item->width();
        items.push_back(item.get());
    }
    if(items.size() < 2 || width - (right - left) < 0.5_px)
        return;
    const pixel_t gap = (width - used) / pixel_t(float(items.size()));
    pixel_t       x   = gap / 2_px;
    for(auto* item : items)
    {
        item->get_el()->pos().x += x - item->left();
        x += item->width() + gap;
    }
}

void litehtml::render_item_inline_context::make_room_for_ruby()
{
    // As Chromium 156: a line whose ruby annotations reach into the line above moves down until their em boxes clear
    // that line's em boxes. The first line's annotations hang over the box's top.
    if(m_line_boxes.size() < 2)
        return;
    const auto& fm    = css().get_font_metrics();
    pixel_t     shift = 0_px;
    for(size_t i = 1; i < m_line_boxes.size(); i++)
    {
        auto& line = m_line_boxes[i];
        if(shift != 0_px)
            line->y_shift(shift);
        bool    found = false;
        pixel_t reach = 0_px;
        for(const auto& item : line->items())
        {
            const auto& el = item->get_el();
            if(item->get_type() != line_box_item::type_text_part)
                continue;
            auto pair = std::dynamic_pointer_cast<render_item_ruby_pair>(el);
            if(!pair && el->src_el()->tag() != _ruby_)
                continue;
            if(pair)
            {
                // A ruby of one pair is that pair.
                if(pair->annotation_overflow > 0_px)
                {
                    const pixel_t top = el->pos().y - pair->annotation_overflow;
                    reach             = found ? std::min(reach, top) : top;
                    found             = true;
                }
                continue;
            }
            for(const auto& child : el->children())
            {
                auto pair = std::dynamic_pointer_cast<render_item_ruby_pair>(child);
                if(!pair || pair->annotation_overflow <= 0_px)
                    continue;
                const pixel_t top = el->pos().y + pair->pos().y - pair->annotation_overflow;
                reach             = found ? std::min(reach, top) : top;
                found             = true;
            }
        }
        if(!found)
            continue;
        const auto&   prev      = m_line_boxes[i - 1];
        const pixel_t em_bottom = prev->bottom() - prev->baseline() + fm.em_bottom();
        if(reach < em_bottom)
        {
            line->y_shift(em_bottom - reach);
            shift += em_bottom - reach;
        }
    }
}

void litehtml::render_item_inline_context::apply_ellipsis(pixel_t room)
{
    const auto tag     = std::dynamic_pointer_cast<html_tag>(src_el());
    std::string mode   = tag ? tag->raw_declaration("text-overflow") : std::string();
    const bool  active = !mode.empty() && lowcase(trim(mode)) == "ellipsis" && css().clips_contents() &&
                        css().get_direction() != direction_rtl;
    // Most boxes have no ellipsis and never had one: their lines are left alone.
    if(!active && !m_cut)
        return;
    m_cut = active;
    m_ellipses.clear();
    auto* container = src_el()->get_document()->container();
    const pixel_t mark = active ? container->text_width("\xE2\x80\xA6", css().get_font()) : 0_px;
    for(const auto& line : m_line_boxes)
    {
        pixel_t right = 0_px;
        for(const auto& item : line->items())
        {
            if(auto text = std::dynamic_pointer_cast<render_text>(item->get_el()))
                text->cut = -1;
            right = std::max(right, item->right());
        }
        if(!active || right <= room)
            continue;
        // Whole characters up to the room the mark leaves, as browsers cut; what follows is not drawn.
        const pixel_t limit = room - mark;
        bool          done  = false;
        position      at(line->left(), line->top(), 0_px, line->height());
        for(const auto& item : line->items())
        {
            auto text = std::dynamic_pointer_cast<render_text>(item->get_el());
            if(done)
            {
                if(text)
                    text->cut = 0;
                continue;
            }
            if(item->right() <= limit)
            {
                at.x = std::max(at.x, item->right());
                continue;
            }
            done = true;
            if(!text || !text->src_el()->parent())
                continue;
            const std::string& shown = text->is_fragment ? text->fragment_text
                                                         : static_cast<const el_text*>(text->src_el().get())->display_text();
            uint_ptr font = text->src_el()->parent()->css().get_font();
            size_t   fit  = 0;
            pixel_t  used = 0_px;
            for(size_t i = 0; i < shown.size();)
            {
                size_t next = i + 1;
                while(next < shown.size() && (static_cast<unsigned char>(shown[next]) & 0xC0) == 0x80)
                    next++;
                pixel_t width = container->text_width(shown.substr(0, next).c_str(), font);
                if(item->left() + width > limit)
                    break;
                fit  = next;
                used = width;
                i    = next;
            }
            text->cut = static_cast<int>(fit);
            at.x      = item->left() + used;
            at.y      = text->pos().y;
            at.height = text->pos().height;
        }
        at.width = mark;
        m_ellipses.push_back(at);
    }
}

void litehtml::render_item_inline_context::draw_children(uint_ptr hdc, pixel_t x, pixel_t y, const position* clip,
                                                         draw_flag flag, int zindex)
{
    render_item_block::draw_children(hdc, x, y, clip, flag, zindex);
    if(flag != draw_inlines || m_ellipses.empty() || !css().get_font())
        return;
    for(position at : m_ellipses)
    {
        at.x += m_pos.x + x;
        at.y += m_pos.y + y;
        src_el()->get_document()->container()->draw_text(hdc, "\xE2\x80\xA6", css().get_font(), css().get_color(), at);
    }
}

void litehtml::render_item_inline_context::fix_line_width(element_float flt, const containing_block_context& self_size,
                                                          formatting_context* fmt_ctx)
{
    if(!m_line_boxes.empty())
    {
        auto el_front = m_line_boxes.back()->get_first_text_part();

        std::vector<std::shared_ptr<render_item>> els;
        bool                                      was_cleared = false;
        if(el_front && el_front->src_el()->css().get_clear() != clear_none)
        {
            if(el_front->src_el()->css().get_clear() == clear_both)
            {
                was_cleared = true;
            } else
            {
                if((flt == float_left && el_front->src_el()->css().get_clear() == clear_left) ||
                   (flt == float_right && el_front->src_el()->css().get_clear() == clear_right))
                {
                    was_cleared = true;
                }
            }
        }

        if(!was_cleared)
        {
            std::list<std::unique_ptr<line_box_item>> items = std::move(m_line_boxes.back()->items());
            m_line_boxes.pop_back();

            for(auto& item : items)
            {
                place_inline(std::move(item), self_size, fmt_ctx);
            }
        } else
        {
            pixel_t line_top = 0_px;
            line_top         = m_line_boxes.back()->top();

            pixel_t line_left  = 0_px;
            pixel_t line_right = self_size.render_width;
            fmt_ctx->get_line_left_right(line_top, self_size.render_width, line_left, line_right);

            if(m_line_boxes.size() == 1)
            {
                if(src_el()->css().get_list_style_type() != list_style_type_none &&
                   src_el()->css().get_list_style_position() == list_style_position_inside)
                {
                    pixel_t sz_font  = src_el()->css().get_font_size();
                    line_left       += sz_font;
                }

                if(pixel_t(src_el()->css().get_text_indent().val()) != 0_px)
                {
                    line_left += src_el()->css().get_text_indent().calc_percent(self_size.width);
                }
            }

            auto items = m_line_boxes.back()->new_width(line_left, line_right);
            for(auto& item : items)
            {
                place_inline(std::move(item), self_size, fmt_ctx);
            }
        }
    }
}

std::list<std::unique_ptr<litehtml::line_box_item>> litehtml::render_item_inline_context::finish_last_box(
    bool end_of_render, const containing_block_context& self_size)
{
    std::list<std::unique_ptr<line_box_item>> ret;

    if(!m_line_boxes.empty())
    {
        ret = m_line_boxes.back()->finish(end_of_render, self_size);

        if(m_line_boxes.back()->is_empty() && end_of_render)
        {
            // remove the last empty line
            m_line_boxes.pop_back();
        } else
        {
            rendered_width line_width = m_line_boxes.back()->get_rendered_width();
            // The first line starts after an inside list marker and the text indent, which its items
            // do not count; without them a max-content width is short and the last word wraps.
            if(m_line_boxes.size() == 1)
            {
                if(src_el()->css().get_list_style_type() != list_style_type_none &&
                   src_el()->css().get_list_style_position() == list_style_position_inside)
                {
                    line_width.natural_width += src_el()->css().get_font_size();
                }
                if(pixel_t(src_el()->css().get_text_indent().val()) != 0_px)
                {
                    line_width.natural_width += src_el()->css().get_text_indent().calc_percent(self_size.width);
                }
            }
            m_rendered_width.merge(line_width);
        }
    }
    return ret;
}

bool litehtml::render_item_inline_context::place_hyphenated(const std::unique_ptr<line_box_item>& item,
                                                           const std::shared_ptr<render_text>& text_item, el_text& text,
                                                           pixel_t minimum, const containing_block_context& self_size,
                                                           formatting_context* fmt_ctx)
{
    const white_space ws = text_item->css().get_white_space();
    if(!m_line_boxes.empty() && m_line_boxes.back()->can_hold(item, ws, m_trailing))
        return false;
    const std::vector<size_t>& points = text.hyphenation_points();
    if(points.empty())
        return false;
    // As Blink: the line takes the word up to its last point where it fits with its hyphen, and the rest goes on to the
    // next line (and so on, for a word longer than a line).
    if(m_line_boxes.empty())
        new_box(item, self_size, fmt_ctx);
    const std::string& content  = text.display_text();
    document_container* cont    = text.get_document()->container();
    const uint_ptr     font     = text_item->css().get_font();
    const pixel_t      trailing = m_trailing;
    const pixel_t      height   = text_item->pos().height;
    auto place = [&](size_t from, size_t to, bool hyphen) {
        auto fragment = std::make_shared<render_text>(text_item->src_el());
        fragment->parent(text_item->parent());
        fragment->is_fragment   = true;
        fragment->hyphen        = hyphen;
        fragment->fragment_text = content.substr(from, to - from);
        const std::string drawn = hyphen ? el_text::hyphenated(fragment->fragment_text) : fragment->fragment_text;
        fragment->pos()         = size(cont->text_width(drawn.c_str(), font), height);
        text_item->fragments.push_back(fragment);
        m_trailing = hyphen ? 0_px : trailing;
        place_inline(std::make_unique<wrapped_text_item>(fragment, minimum), self_size, fmt_ctx);
    };
    for(size_t start = 0;;)
    {
        const line_box& line = *m_line_boxes.back();
        if(auto last = line.get_last_text_part(); last && last->src_el()->is_break() && last->css().get_clear() == clear_none)
        {
            new_box(item, self_size, fmt_ctx);  // after a <br>: a line of its own
            continue;
        }
        const pixel_t room = line.line_right() - line.right();
        const pixel_t rest = start == 0 ? text_item->pos().width : cont->text_width(content.c_str() + start, font);
        size_t        cut  = 0;
        if(rest + trailing > room)
            for(size_t point : points)
            {
                if(point <= start || point >= content.size())
                    continue;
                if(cont->text_width(el_text::hyphenated(content.substr(start, point - start)).c_str(), font) > room)
                    break;  // nor any longer part
                cut = point;
            }
        if(cut)
        {
            place(start, cut, true);
            new_box(item, self_size, fmt_ctx);
            start = cut;
        } else if(rest + trailing <= room || line.is_empty())
        {
            if(start == 0)
                return false;  // whole: it fits, or not even its first part fits a line
            place(start, content.size(), false);
            break;
        } else
            new_box(item, self_size, fmt_ctx);
    }
    m_trailing = trailing;
    m_fragmented_texts.push_back(text_item);
    return true;
}

namespace
{
    // The first character `el` shows (0: none, or not within `budget` elements): leading ASCII white space skipped,
    // annotations (rt, rp) left out.
    char32_t leading_char(const litehtml::element::ptr& el, int& budget)
    {
        if(!el || --budget < 0 || el->css().get_display() == litehtml::display_none || el->tag() == litehtml::_rt_ ||
           el->tag() == litehtml::_rp_)
            return 0;
        if(el->is_text())
        {
            std::string text;
            el->get_text(text);
            const size_t at = text.find_first_not_of(" \t\r\n\f");
            return at == std::string::npos ? 0 : first_char(text.substr(at));
        }
        for(const auto& child : el->children())
            if(char32_t c = leading_char(child, budget))
                return c;
        return 0;
    }
} // namespace

bool litehtml::render_item_inline_context::paragraphs_indented()
{
    if(m_paragraphs_indented >= 0)
        return m_paragraphs_indented != 0;
    constexpr char32_t ideographic_space = 0x3000;
    const element::ptr el                = src_el();
    bool               yes               = false;
    // Its own lines after a <br> (a chapter of lines, as calibre makes them).
    {
        int  budget = 4000;
        bool after  = false;
        std::function<bool(const element::ptr&)> walk = [&](const element::ptr& e) {
            if(--budget < 0)
                return false;
            if(e->is_break())
            {
                after = true;
                return false;
            }
            if(e->is_text())
            {
                int  one = 1;
                const char32_t c = leading_char(e, one);
                if(!c)
                    return false;
                const bool indented = after && c == ideographic_space;
                after               = false;
                return indented;
            }
            if(e->tag() == _rt_ || e->tag() == _rp_)
                return false;
            for(const auto& child : e->children())
                if(walk(child))
                    return true;
            return false;
        };
        yes = walk(el);
    }
    // Its nearest sibling paragraphs.
    if(!yes && el->parent())
    {
        const auto& kids = el->parent()->children();
        const auto  self = std::find(kids.begin(), kids.end(), el);
        auto        indented = [&](const element::ptr& s) {
            if(s->is_text() || s->tag() != el->tag())
                return false;
            if(s->css().get_text_indent().val() > 0)
                return true;
            int budget = 200;
            return leading_char(s, budget) == ideographic_space;
        };
        if(self != kids.end())
        {
            int n = 0;
            for(auto it = self; it != kids.begin() && n < 8 && !yes;)
                if(!(*--it)->is_text() && ++n)
                    yes = indented(*it);
            n = 0;
            for(auto it = std::next(self); it != kids.end() && n < 8 && !yes; ++it)
                if(!(*it)->is_text() && ++n)
                    yes = indented(*it);
        }
    }
    m_paragraphs_indented = yes ? 1 : 0;
    return yes;
}

litehtml::pixel_t litehtml::render_item_inline_context::new_box(const std::unique_ptr<line_box_item>& el,
                                                                const containing_block_context&       self_size,
                                                                formatting_context*                   fmt_ctx)
{
    // A paragraph starts on the block's first line and on the line after a <br> (JLReq 3.1.5: 改行行頭).
    bool paragraph = m_line_boxes.empty();
    if(!paragraph)
    {
        const auto last = m_line_boxes.back()->get_last_text_part();
        paragraph       = last && last->src_el()->is_break();
    }
    auto    items    = finish_last_box(false, self_size);
    pixel_t line_top = 0_px;
    if(!m_line_boxes.empty())
    {
        line_top = m_line_boxes.back()->bottom();
    }
    line_top = fmt_ctx->get_cleared_top(el->get_el(), line_top);

    pixel_t first_line_margin = 0_px;
    pixel_t text_indent       = 0_px;
    if(m_line_boxes.empty())
    {
        if(src_el()->css().get_list_style_type() != list_style_type_none &&
           src_el()->css().get_list_style_position() == list_style_position_inside)
        {
            first_line_margin = src_el()->css().get_font_size();
        }
        if(pixel_t(src_el()->css().get_text_indent().val()) != 0_px)
        {
            text_indent = src_el()->css().get_text_indent().calc_percent(self_size.width);
        }
    }

    formatting_context::el_position el_pos;
    el_pos.container_width  = self_size.render_width;
    el_pos.el_pos           = el->pos();
    el_pos.el_pos.y         = line_top;
    el_pos.el_pos.x         = 0;
    el_pos.el_pos.width    += text_indent + first_line_margin;
    auto new_pos            = fmt_ctx->place_to_left(el_pos);

    m_line_boxes.emplace_back(std::make_unique<line_box>(new_pos.top, new_pos.left + first_line_margin + text_indent,
                                                         new_pos.left + new_pos.width, css().line_height(),
                                                         css().get_font_metrics(), css().get_text_align(),
                                                         css().get_direction() == direction_rtl));
    // JLReq 3.1.5: an opening bracket that starts a paragraph. With an indent of its own of an em or more, the
    // bracket's first half goes (as at any line's start). With less, in a book that indents its paragraphs, the bracket
    // keeps it: in ① (the default) after the half em less the indent, so a full em is before its ink (全角アキ); in ③
    // with nothing more (二分アキ, document::set_paragraph_brackets). Not for a centred or end-aligned line, nor a
    // negative (hanging) indent.
    if(paragraph && is_one_of(css().get_text_align(), text_align_left, text_align_justify, text_align_start))
    {
        const pixel_t em = css().get_font_size();
        if(text_indent >= 0_px && text_indent < em && first_line_margin == 0_px && paragraphs_indented())
        {
            const bool full_em = src_el()->get_document()->get_paragraph_brackets() == paragraph_brackets::full_em;
            m_line_boxes.back()->set_paragraph_start(full_em ? std::max(0_px, em / 2_px - text_indent) : 0_px);
        }
    }

    // Add items returned by finish_last_box function into the new line
    for(auto& it : items)
    {
        m_line_boxes.back()->add_item(std::move(it));
    }

    return new_pos.top;
}

void litehtml::render_item_inline_context::place_inline(std::unique_ptr<line_box_item>  item,
                                                        const containing_block_context& self_size,
                                                        formatting_context*             fmt_ctx)
{
    if(item->get_el()->src_el()->css().get_display() == display_none)
    {
        return;
    }

    if(item->get_el()->src_el()->is_float())
    {
        pixel_t line_top = 0_px;
        if(!m_line_boxes.empty())
        {
            line_top = m_line_boxes.back()->top();
        }
        auto rw = place_float(item->get_el(), line_top, self_size, fmt_ctx);
        m_rendered_width.merge(rw);
        return;
    }

    if(item->get_type() == line_box_item::type_text_part)
    {
        if(item->get_el()->src_el()->is_inline_box())
        {
            pixel_t min_rendered_width = item->get_el()->render(0_px, 0_px, self_size, fmt_ctx).natural_width;
            if(min_rendered_width < item->get_el()->width() &&
               item->get_el()->src_el()->css().get_width().is_predefined())
            {
                item->get_el()->render(0_px, 0_px, self_size.new_width(min_rendered_width), fmt_ctx);
            }
            item->set_rendered_min_width(min_rendered_width);
        } else if(item->get_el()->src_el()->css().get_display() == display_inline_text)
        {
            auto text_item = std::dynamic_pointer_cast<render_text>(item->get_el());
            if(text_item && !text_item->is_fragment)
            {
                text_item->fragments.clear();
                text_item->hyphen = false;
                text_item->justify_gap = 0_px;
                text_item->trims.clear();
                text_item->ruby_over_start = text_item->ruby_over_end = 0_px;
                litehtml::size sz;
                item->get_el()->src_el()->get_content_size(sz, self_size.render_width);
                item->get_el()->pos() = sz;
                text_item->compress(cjk::line_middle, cjk::line_middle);
                item->set_rendered_min_width(sz.width);
                auto text = std::dynamic_pointer_cast<el_text>(item->get_el()->src_el());
                const auto wrapping = text_item->css().get_wrapping();
                const auto whitespace = text_item->css().get_white_space();
                if(text && !text->is_white_space() && !text->is_break() && whitespace != white_space_nowrap &&
                   whitespace != white_space_pre && place_hyphenated(item, text_item, *text, sz.width, self_size, fmt_ctx))
                    return;
                if(text && !text->is_space() && !text->is_break() && wrapping != overflow_wrap_normal &&
                   whitespace != white_space_nowrap && whitespace != white_space_pre)
                {
                    pixel_t line_left = 0_px;
                    pixel_t line_right = self_size.render_width;
                    fmt_ctx->get_line_left_right(m_line_boxes.empty() ? 0_px : m_line_boxes.back()->top(),
                        self_size.render_width, line_left, line_right);
                    if(m_line_boxes.empty())
                    {
                        line_left += src_el()->css().get_text_indent().calc_percent(self_size.width);
                        if(src_el()->css().get_list_style_type() != list_style_type_none &&
                           src_el()->css().get_list_style_position() == list_style_position_inside)
                            line_left += src_el()->css().get_font_size();
                    } else
                        line_left = std::max(line_left, m_line_boxes.back()->left());
                    const pixel_t line_width = line_right - line_left;
                    auto breaker = wrapping == overflow_wrap_anywhere || sz.width + m_trailing > line_width ? text->breaker() : nullptr;
                    if(breaker)
                    {
                        const pixel_t minimum = wrapping == overflow_wrap_anywhere ? breaker->minimum_width() : sz.width;
                        item = std::make_unique<wrapped_text_item>(text_item, minimum);
                        if(sz.width + m_trailing > line_width)
                        {
                            const std::string& content = text->display_text();
                            const bool needs_line = m_line_boxes.empty() || !m_line_boxes.back()->can_hold(item, whitespace, m_trailing);
                            text_item->pos().width = breaker->minimum_width();
                            if(needs_line)
                                new_box(item, self_size, fmt_ctx);
                            const pixel_t trailing = m_trailing;
                            for(size_t start = 0; start < content.size();)
                            {
                                pixel_t available = m_line_boxes.back()->line_right() - m_line_boxes.back()->right();
                                size_t end = breaker->fit(start, available);
                                if(end <= start || end > content.size())
                                    break;
                                if(end == content.size() && trailing > 0_px)
                                    end = breaker->fit(start, available - trailing);
                                auto fragment = std::make_shared<render_text>(text_item->src_el());
                                fragment->parent(text_item->parent());
                                fragment->is_fragment = true;
                                fragment->fragment_text = content.substr(start, end - start);
                                fragment->pos() = size(text->doc_ptr()->container()->text_width(fragment->fragment_text.c_str(), text->css().get_font()), sz.height);
                                text_item->fragments.push_back(fragment);
                                m_trailing = end == content.size() ? trailing : 0_px;
                                place_inline(std::make_unique<wrapped_text_item>(fragment, minimum), self_size, fmt_ctx);
                                start = end;
                                if(start < content.size())
                                    new_box(item, self_size, fmt_ctx);
                            }
                            m_trailing = trailing;
                            if(!text_item->fragments.empty())
                            {
                                m_fragmented_texts.push_back(text_item);
                                return;
                            }
                            text_item->pos() = sz;
                            text_item->trims.clear();
                            text_item->compress(cjk::line_middle, cjk::line_middle);
                        }
                    }
                }
            }
        }
    }

    bool add_box = true;

    if(!m_line_boxes.empty())
    {
        if(m_line_boxes.back()->can_hold(item, src_el()->css().get_white_space(), item->get_type() == line_box_item::type_text_part ? m_trailing : 0_px))
        {
            add_box = false;
        }
    }
    if(add_box)
    {
        new_box(item, self_size, fmt_ctx);
    }

    if(!item->get_el()->src_el()->is_inline())
    {
        if(m_line_boxes.size() == 1)
        {
            if(collapse_top_margin())
            {
                pixel_t shift = item->get_el()->margin_top();
                if(shift >= 0_px)
                {
                    m_line_boxes.back()->y_shift(-shift);
                }
            }
        } else
        {
            pixel_t shift = 0_px;

            pixel_t prev_margin = m_line_boxes[m_line_boxes.size() - 2]->bottom_margin();

            if(prev_margin > item->get_el()->margin_top())
            {
                shift = item->get_el()->margin_top();
            } else
            {
                shift = prev_margin;
            }
            if(shift >= 0_px)
            {
                m_line_boxes.back()->y_shift(-shift);
            }
        }
    }

    m_line_boxes.back()->add_item(std::move(item));
}

void litehtml::render_item_inline_context::apply_vertical_align()
{
    if(!m_line_boxes.empty())
    {
        pixel_t add = 0_px;

        pixel_t content_height = m_line_boxes.back()->bottom() - m_va_applied;

        if(m_pos.height > content_height)
        {
            switch(src_el()->css().get_vertical_align())
            {
            case va_middle:
                add = (m_pos.height - content_height) / 2;
                break;
            case va_bottom:
                add = m_pos.height - content_height;
                break;
            default:
                add = 0_px;
                break;
            }
        }

        if(add == m_va_applied)
            return;
        pixel_t shift = add;
        if(m_va_applied != 0_px)
        {
            if(last_layout_valid())
            {
                const position placed = m_pos;
                m_layout_dirty        = true;
                const layout_cache c  = *m_cache;
                render(c.last_x, c.last_y, c.last.cb, nullptr, c.last.second_pass);
                m_pos = placed;
            } else
                shift = add - m_va_applied;
        }
        if(shift != 0_px)
        {
            for(auto& box : m_line_boxes)
            {
                box->y_shift(shift);
            }
        }
        m_va_applied = add;
        for(const auto& text : m_fragmented_texts)
            text->update_fragment_bounds();
    }
}

litehtml::pixel_t litehtml::render_item_inline_context::control_baseline_inset() const
{
    if(src_el()->tag() != _input_)
    {
        return 0_px;
    }
    const char* type = src_el()->get_attr("type");
    return type && (!t_strcasecmp(type, "checkbox") || !t_strcasecmp(type, "radio")) ? 2_px : 0_px;
}

litehtml::pixel_t litehtml::render_item_inline_context::get_first_baseline()
{
    pixel_t bl;
    if(!m_line_boxes.empty())
    {
        const auto& line = m_line_boxes.front();
        bl               = line->bottom() - line->baseline() + content_offset_top();
    } else
    {
        bl = height() - margin_bottom() - control_baseline_inset();
    }
    return bl;
}

litehtml::pixel_t litehtml::render_item_inline_context::get_last_baseline()
{
    pixel_t bl;
    if(!m_line_boxes.empty())
    {
        const auto& line = m_line_boxes.back();
        bl               = line->bottom() - line->baseline() + content_offset_top();
    } else
    {
        bl = src_el()->tag() == _input_ ? height() - margin_bottom() - control_baseline_inset() : height();
    }
    return bl;
}
