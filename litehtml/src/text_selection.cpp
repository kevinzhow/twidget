#include "html.h"
#include "text_selection.h"

#include "document_container.h"
#include "el_text.h"
#include "render_inline.h"
#include "render_item.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <unordered_map>

namespace litehtml
{
    namespace
    {
        bool continuation(const std::string& s, size_t i) { return i < s.size() && (uint8_t(s[i]) & 0xC0) == 0x80; }
    } // namespace

    text_selection::text_selection(document& doc) :
        m_doc(doc)
    {
        int blocks = 0;
        std::function<void(const element::ptr&, int, bool)> walk = [&](const element::ptr& el, int block, bool annotation) {
            if(!el->is_text())
            {
                if(el->css().get_display() == display_none)
                    return;
                // An annotation flows in its own block box but belongs to its base's paragraph.
                if(el->tag() == _rt_)
                    annotation = true;
                else if(!el->is_inline())
                    block = ++blocks;
                for(const auto& child : el->children())
                    walk(child, block, annotation);
                return;
            }
            auto text = std::dynamic_pointer_cast<el_text>(el);
            if(!text || !el->parent())
                return;
            size_t offset = 0;
            el->for_each_render_item([&](const std::shared_ptr<render_item>& ri) {
                // White space the layout dropped (collapsed, or a removed segment break) is not in the text.
                if(ri->skip())
                    return;
                position placed = ri->get_placement();
                auto     item   = std::dynamic_pointer_cast<render_text>(ri);
                auto add = [&](const std::string& shown, const position& box, pixel_t gap, const std::vector<char_trim>& trims) {
                    if(shown.empty())
                        return;
                    m_runs.push_back({text, offset, shown, box, block, annotation, gap, trims});
                    offset += shown.size();
                };
                if(item && !item->fragments.empty())
                {
                    for(const auto& fragment : item->fragments)
                    {
                        position box  = fragment->pos();
                        box.x        += placed.x - ri->pos().x;
                        box.y        += placed.y - ri->pos().y;
                        add(fragment->fragment_text, box, fragment->justify_gap, {});
                    }
                } else
                {
                    add(text->display_text(), placed, item ? item->justify_gap : 0_px, item ? item->trims : std::vector<char_trim>());
                }
            });
        };
        if(doc.root())
            walk(doc.root(), 0, false);
        // An element's runs are consecutive (its render items one after another): where they start.
        for(size_t i = 0; i < m_runs.size(); i++)
            m_first_run.emplace(m_runs[i].text.get(), i);
    }

    position text_selection::to_page(const position& box) const
    {
        if(!m_doc.vertical())
            return box;
        position v;
        m_doc.container()->get_viewport(v);
        return position(v.width - box.y - box.height, box.x, box.height, box.width);
    }

    void text_selection::to_layout(pixel_t x, pixel_t y, pixel_t& lx, pixel_t& ly) const
    {
        lx = x;
        ly = y;
        if(!m_doc.vertical())
            return;
        position v;
        m_doc.container()->get_viewport(v);
        lx = y;
        ly = v.width - x;
    }

    pixel_t text_selection::advance(const text_run& run, size_t bytes, bool after_gap) const
    {
        if(bytes == 0)
            return 0_px;
        if(bytes >= run.shown.size())
            return run.box.width;
        const pixel_t spread = justify_shift(run.shown, bytes, run.justify_gap, after_gap) - trim_shift(run.trims, bytes);
        return run.text->advance_to(run.shown, bytes) + spread;
    }

    text_position text_selection::position_at(pixel_t x, pixel_t y) const
    {
        pixel_t lx, ly;
        to_layout(x, y, lx, ly);
        const text_run* best = nullptr;
        pixel_t         best_dy = 0_px, best_dx = 0_px;
        for(const auto& r : m_runs)
        {
            if(r.annotation)
                continue;
            pixel_t dy = ly < r.box.y ? r.box.y - ly : ly > r.box.bottom() ? ly - r.box.bottom() : 0_px;
            pixel_t dx = lx < r.box.x ? r.box.x - lx : lx > r.box.right() ? lx - r.box.right() : 0_px;
            if(!best || dy < best_dy || (dy == best_dy && dx < best_dx))
            {
                best    = &r;
                best_dy = dy;
                best_dx = dx;
            }
        }
        if(!best)
            return {};
        // The character boundary nearest the point along the line.
        const pixel_t target = lx - best->box.x;
        size_t        bytes = 0;
        pixel_t       prev  = 0_px;
        for(size_t i = 1; i <= best->shown.size(); i++)
        {
            if(continuation(best->shown, i))
                continue;
            // A boundary at a justified gap is nearest in the middle of the gap.
            pixel_t a = (advance(*best, i, false) + advance(*best, i, true)) / 2_px;
            if(a >= target)
            {
                bytes = target - prev <= a - target ? bytes : i;
                return {best->text, best->offset + bytes};
            }
            bytes = i;
            prev  = a;
        }
        return {best->text, best->offset + best->shown.size()};
    }

    bool text_selection::locate(const text_position& p, size_t& run, size_t& offset) const
    {
        bool found = false;
        // Only the element's own runs (a linear scan of every run made a page's selection quadratic in its text).
        auto first = m_first_run.find(p.text.get());
        if(first == m_first_run.end())
            return false;
        for(size_t i = first->second; i < m_runs.size() && m_runs[i].text == p.text; i++)
        {
            const auto& r = m_runs[i];
            if(p.offset < r.offset || p.offset > r.offset + r.shown.size())
                continue;
            run    = i;
            offset = p.offset - r.offset;
            found  = true;
            // A run's end is the next one's start: the next one takes it.
            if(offset < r.shown.size())
                return true;
        }
        return found;
    }

    bool text_selection::ordered(const text_position& a, const text_position& b, size_t& r0, size_t& o0, size_t& r1,
                                 size_t& o1) const
    {
        if(!locate(a, r0, o0) || !locate(b, r1, o1))
            return false;
        if(std::tie(r0, o0) > std::tie(r1, o1))
        {
            std::swap(r0, r1);
            std::swap(o0, o1);
        }
        return true;
    }

    std::vector<position> text_selection::rects(const text_position& a, const text_position& b) const
    {
        std::vector<position> out;
        size_t r0, o0, r1, o1;
        if(!ordered(a, b, r0, o0, r1, o1))
            return out;
        bool    open = false;
        position line;
        for(size_t i = r0; i <= r1; i++)
        {
            const auto& r = m_runs[i];
            size_t from = i == r0 ? o0 : 0, to = i == r1 ? o1 : r.shown.size();
            if(r.annotation || from >= to)
                continue;
            const pixel_t start = advance(r, from, true), end = advance(r, to, false);
            position      part(r.box.x + start, r.box.y, end - start, r.box.height);
            // Runs of one line join: the same row (a ruby pair's centring leaves gaps between them).
            if(open && std::abs(float(part.y - line.y)) < float(line.height) / 2 && part.x >= line.x)
            {
                pixel_t top = std::min(line.y, part.y), bottom = std::max(line.bottom(), part.bottom());
                line.width  = std::max(line.right(), part.right()) - line.x;
                line.y      = top;
                line.height = bottom - top;
                continue;
            }
            if(open)
                out.push_back(to_page(line));
            line = part;
            open = true;
        }
        if(open)
            out.push_back(to_page(line));
        return out;
    }

    std::string text_selection::text(const text_position& a, const text_position& b) const
    {
        std::string out;
        size_t r0, o0, r1, o1;
        if(!ordered(a, b, r0, o0, r1, o1))
            return out;
        int block = -1;
        for(size_t i = r0; i <= r1; i++)
        {
            const auto& r = m_runs[i];
            size_t from = i == r0 ? o0 : 0, to = i == r1 ? o1 : r.shown.size();
            if(r.annotation || from >= to)
                continue;
            if(block >= 0 && r.block != block && !out.empty() && out.back() != '\n')
                out += '\n';
            block = r.block;
            out.append(r.shown, from, to - from);
        }
        return out;
    }

    bool text_selection::paragraph(const text_position& at, text_position& start, text_position& end) const
    {
        size_t run, offset;
        if(!locate(at, run, offset))
            return false;
        const int block = m_runs[run].block;
        size_t    first = run, last = run;
        while(first > 0 && m_runs[first - 1].block == block)
            first--;
        while(last + 1 < m_runs.size() && m_runs[last + 1].block == block)
            last++;
        // Annotations at either end are not the paragraph's text.
        while(first < last && m_runs[first].annotation)
            first++;
        while(last > first && m_runs[last].annotation)
            last--;
        start = {m_runs[first].text, m_runs[first].offset};
        end   = {m_runs[last].text, m_runs[last].offset + m_runs[last].shown.size()};
        return true;
    }

    bool text_selection::sentence(const text_position& at, text_position& start, text_position& end) const
    {
        text_position p0, p1;
        size_t        run, offset, r0, o0, r1, o1;
        if(!paragraph(at, p0, p1) || !locate(at, run, offset) || !ordered(p0, p1, r0, o0, r1, o1))
            return false;
        // The paragraph's base text, each byte mapped back to its run.
        std::string                           text;
        std::vector<std::pair<size_t, size_t>> where;
        size_t                                at_index = 0;
        for(size_t i = r0; i <= r1; i++)
        {
            const auto& r = m_runs[i];
            if(r.annotation)
                continue;
            if(i == run)
                at_index = text.size() + offset;
            for(size_t k = 0; k < r.shown.size(); k++)
                where.emplace_back(i, k);
            text += r.shown;
        }
        if(text.empty())
            return false;
        auto starts = [&](size_t i, std::initializer_list<const char*> list) {
            for(const char* s : list)
                if(text.compare(i, std::strlen(s), s) == 0)
                    return std::strlen(s);
            return size_t(0);
        };
        auto stop    = [&](size_t i) { return starts(i, {"\xE3\x80\x82", "\xEF\xBC\x81", "\xEF\xBC\x9F", "!", "?", "."}); };
        auto closing = [&](size_t i) {
            return starts(i, {"\xE3\x80\x8D", "\xE3\x80\x8F", "\xEF\xBC\x89", "\xE3\x80\x91", "\xE2\x80\x9D", ")", "\""});
        };
        // A sentence ends after its stop and the closing brackets that follow it.
        auto end_after = [&](size_t i) {
            size_t n = stop(i);
            if(!n)
                return size_t(0);
            i += n;
            while(i < text.size())
            {
                size_t c = closing(i);
                if(!c)
                    break;
                i += c;
            }
            return i;
        };
        size_t s = 0;
        for(size_t i = 0; i < at_index && i < text.size(); i++)
            if(size_t e = end_after(i); e && e <= at_index)
                s = e;
        size_t e = text.size();
        for(size_t i = std::max(s, at_index); i < text.size(); i++)
            if(size_t after = end_after(i))
            {
                e = after;
                break;
            }
        // Spaces and ideographic spaces before the sentence are not in it.
        while(s < e && (text[s] == ' ' || text[s] == '\n' || text.compare(s, 3, "\xE3\x80\x80") == 0))
            s += text[s] == ' ' || text[s] == '\n' ? 1 : 3;
        auto at_byte = [&](size_t i) -> text_position {
            if(i >= where.size())
            {
                const auto& r = m_runs[where.back().first];
                return {r.text, r.offset + r.shown.size()};
            }
            const auto& r = m_runs[where[i].first];
            return {r.text, r.offset + where[i].second};
        };
        start = at_byte(s);
        end   = e > 0 && e <= where.size() ? text_position{m_runs[where[e - 1].first].text,
                                                         m_runs[where[e - 1].first].offset + where[e - 1].second + 1}
                                         : at_byte(e);
        return true;
    }
} // namespace litehtml
