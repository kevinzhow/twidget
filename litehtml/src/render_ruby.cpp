#include "render_ruby.h"

#include "document.h"
#include "html_tag.h"
#include "render_inline_context.h"
#include "el_text.h"
#include "cjk_rules.h"

std::shared_ptr<litehtml::render_item> litehtml::render_item_ruby::init()
{
    // The children as the DOM gives them: base content (text, <rb>, spans), <rt> blocks, <rp> already gone
    // (display: none makes no render item). Each <rt> closes a pair with the base content before it; base
    // content after the last <rt> is a pair of its own, with no annotation.
    std::list<std::shared_ptr<render_item>> pairs;
    std::list<std::shared_ptr<render_item>> base;
    auto self = shared_from_this();
    // One pair (most rubies): the ruby is that pair. Its box is the pair's (the inline formatting context around a
    // single inline-block adds nothing to it), so the pair needs no element and the ruby no context of its own.
    size_t count = 0;
    bool   trailing = false;
    for(const auto& child : m_children)
    {
        if(child->src_el()->tag() == _rt_ && !child->src_el()->is_inline())
        {
            count++;
            trailing = false;
        } else if(!child->src_el()->is_white_space())
            trailing = true;
    }
    const bool single = count + (trailing ? 1 : 0) == 1 && src_el()->css().get_display() == display_inline_block;
    auto make_pair = [&](const std::shared_ptr<render_item>& rt) {
        bool content = false;
        for(const auto& b : base)
            if(!b->src_el()->is_white_space())
                content = true;
        if(!content && !rt)
            return;
        auto pair = std::make_shared<render_item_ruby_pair>(
            single ? src_el() : std::make_shared<html_tag>(src_el(), "display:inline-block;text-indent:0"));
        pair->parent(single ? parent() : self);
        if(rt)
        {
            pair->add_child(rt);
            pair->has_annotation = true;
        }
        auto base_el = std::make_shared<html_tag>(src_el(), "display:block;text-align:center;text-indent:0");
        auto base_ri = std::make_shared<render_item_block>(base_el);
        for(const auto& b : base)
            if(content)
                base_ri->add_child(b);
        if(!content)
        {
            // An annotation over nothing still takes the line's height for its base.
            base_el = std::make_shared<html_tag>(src_el(), "display:block;text-indent:0;white-space:pre");
            base_ri = std::make_shared<render_item_block>(base_el);
        }
        pair->add_child(base_ri);
        pairs.push_back(pair);
        base.clear();
    };
    for(const auto& child : m_children)
    {
        if(child->src_el()->tag() == _rt_ && !child->src_el()->is_inline())
            make_pair(child);
        else
            base.push_back(child);
    }
    make_pair(nullptr);
    if(single && pairs.size() == 1)
    {
        auto ret = pairs.front();
        for(auto& el : ret->children())
        {
            el = el->init();
            el->parent(ret);
        }
        src_el()->add_render(ret);
        return ret;
    }
    m_children = pairs;
    // A word (熟語ルビ) of several pairs may break between them (JLReq 3.3.7; render_item_ruby_word decides at layout,
    // by the document's ruby_breaking).
    const bool word = pairs.size() > 1;
    for(auto& pair : m_children)
    {
        for(auto& el : pair->children())
            el = el->init();
        pair->src_el()->add_render(pair);
        if(auto p = std::dynamic_pointer_cast<render_item_ruby_pair>(pair))
            p->word_member = word;
    }
    // The pairs are inline-blocks: the ruby is an inline formatting context of them.
    std::shared_ptr<render_item_inline_context> ret = word ? std::make_shared<render_item_ruby_word>(src_el())
                                                           : std::make_shared<render_item_inline_context>(src_el());
    ret->parent(parent());
    ret->children() = m_children;
    for(const auto& el : ret->children())
        el->parent(ret);
    ret->src_el()->add_render(ret);
    return ret;
}

bool litehtml::render_item_ruby_word::split() const
{
    return src_el()->get_document()->get_ruby_breaking() == ruby_breaking::per_base;
}

bool litehtml::render_item_ruby_pair::in_word() const
{
    return word_member && src_el()->get_document()->get_ruby_breaking() == ruby_breaking::per_base;
}

void litehtml::render_item_ruby_pair::align_base(pixel_t shift)
{
    if(m_children.size() < 2 || shift == base_shift)
        return;
    m_children.back()->pos().x += shift - base_shift;
    base_shift                  = shift;
}

litehtml::rendered_width litehtml::render_item_ruby_pair::_render_content(pixel_t x, pixel_t y, bool second_pass,
                                                                       const containing_block_context& self_size,
                                                                       formatting_context* fmt_ctx)
{
    auto ret            = render_item_block_context::_render_content(x, y, second_pass, self_size, fmt_ctx);
    annotation_overflow = 0_px;
    base_start = base_end = ruby_em = base_shift = overlap_before = ann_start = ann_end = 0_px;
    japanese_base = false;
    if(!has_annotation || m_children.size() < 2)
        return ret;
    // ruby-align: space-around, Chromium's default: the narrower of the two spreads over the pair.
    for(const auto& el : m_children)
        if(auto inl = std::dynamic_pointer_cast<render_item_inline_context>(el))
            inl->space_around(el->pos().width);
    // Where the base's text is in the pair, for the overhang (line_box).
    {
        const auto& b = m_children.back();
        bool        any = false;
        if(auto inl = std::dynamic_pointer_cast<render_item_inline_context>(b); inl && inl->line_boxes().size() == 1)
            for(const auto& item : inl->line_boxes().front()->items())
            {
                if(item->get_type() != line_box_item::type_text_part || item->get_el()->src_el()->is_white_space())
                    continue;
                if(!any)
                    if(auto text = std::dynamic_pointer_cast<el_text>(item->get_el()->src_el()); text && !text->display_text().empty())
                    {
                        const std::string& s  = text->display_text();
                        const unsigned char c0 = s[0];
                        const size_t        n  = c0 < 0x80 ? 1 : (c0 >> 5) == 6 ? 2 : (c0 >> 4) == 14 ? 3 : 4;
                        char32_t            c  = n == 1 ? c0 : n == 2 ? c0 & 0x1F : n == 3 ? c0 & 0x0F : c0 & 0x07;
                        for(size_t k = 1; k < n && k < s.size(); k++)
                            c = (c << 6) | (static_cast<unsigned char>(s[k]) & 0x3F);
                        japanese_base = cjk::script_of(c) == cjk::script::japanese;
                    }
                base_start = any ? std::min(base_start, item->left()) : item->left();
                base_end   = any ? std::max(base_end, item->right()) : item->right();
                any        = true;
            }
        if(any)
        {
            base_start += b->pos().x;
            base_end   += b->pos().x;
            ruby_em     = m_children.front()->css().get_font_size();
        } else
            base_start = base_end = 0_px;
    }
    // Where the annotation's text is in the pair (JLReq 3.3.7: what a longer reading next to it may overhang).
    const pixel_t whole = m_children.front()->pos().x + m_children.front()->pos().width;
    ann_start = 0_px;
    ann_end   = whole;
    if(auto inl = std::dynamic_pointer_cast<render_item_inline_context>(m_children.front()); inl && inl->line_boxes().size() == 1)
    {
        bool any = false;
        for(const auto& item : inl->line_boxes().front()->items())
        {
            if(item->get_type() != line_box_item::type_text_part || item->get_el()->src_el()->is_white_space())
                continue;
            ann_start = any ? std::min(ann_start, item->left()) : item->left();
            ann_end   = any ? std::max(ann_end, item->right()) : item->right();
            any       = true;
        }
        if(any)
        {
            ann_start += m_children.front()->pos().x;
            ann_end   += m_children.front()->pos().x;
        } else
        {
            ann_start = 0_px;
            ann_end   = whole;
        }
    }
    // As Chromium 156 measures it: the annotation's box (its line, line-height: normal) ends where the base's em box
    // begins. The pair's own box is the base's: the annotation hangs over it, and the inline formatting context
    // makes room for it between lines (render_item_inline_context::make_room_for_ruby).
    const auto&   rt      = m_children.front();
    const auto&   base    = m_children.back();
    const auto&   rt_fm   = rt->src_el()->css().get_font_metrics();
    const auto&   base_fm = base->src_el()->css().get_font_metrics();
    const pixel_t base_em_top = base->top() + base->get_first_baseline() - base_fm.em_top();
    base->y_shift(rt->bottom() - base_em_top);
    const pixel_t top = base->top();
    for(const auto& el : m_children)
        el->y_shift(-top);
    annotation_overflow = -(rt->top() + rt->get_first_baseline() - rt_fm.em_top());
    m_pos.height        = base->bottom();
    return ret;
}
