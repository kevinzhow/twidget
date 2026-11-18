#include "html.h"
#include "document.h"
#include "el_space.h"

litehtml::el_space::el_space(const char* text, const std::shared_ptr<document>& doc) :
    el_text(text, doc)
{
}

void litehtml::el_space::compute_styles(bool recursive)
{
    // A run of white space is one element where it collapses. Where its characters are kept (pre, pre-wrap) or its
    // line breaks are (pre-line), each is laid out on its own (a tab, a line break, a break after each space): the
    // run becomes one element per character.
    auto par = parent();
    if(m_text.size() > 1 && par &&
       is_one_of(par->css().get_white_space(), white_space_pre, white_space_pre_wrap, white_space_pre_line))
    {
        elements_list rest;
        for(size_t i = 1; i < m_text.size(); i++)
        {
            auto space = std::make_shared<el_space>(m_text.substr(i, 1).c_str(), get_document());
            space->set_source(m_src_pos, m_src_utf16 + static_cast<uint32_t>(i));
            rest.push_back(space);
        }
        set_text(m_text.substr(0, 1));
        par->insert_after(shared_from_this(), rest);
        for(const auto& el : rest)
            el->compute_styles(recursive);
    }
    el_text::compute_styles(recursive);
}

bool litehtml::el_space::is_white_space() const
{
    white_space ws = css().get_white_space();
    return ws == white_space_normal || ws == white_space_nowrap || ws == white_space_pre_line;
}

bool litehtml::el_space::is_break() const
{
    white_space ws = css().get_white_space();
    if(ws == white_space_pre || ws == white_space_pre_line || ws == white_space_pre_wrap)
    {
        if(m_text == "\n")
        {
            return true;
        }
    }
    return false;
}

bool litehtml::el_space::is_space() const
{
    return true;
}

std::string litehtml::el_space::dump_get_name()
{
    return "space: \"" + get_escaped_string(m_text) + "\"";
}
