#include "el_break.h"

litehtml::el_break::el_break(const std::shared_ptr<litehtml::document>& doc) :
    html_tag(doc)
{
}

bool litehtml::el_break::is_break() const
{
    return true;
}

void litehtml::el_break::compute_styles(bool recursive)
{
    html_tag::compute_styles(recursive);
    // litehtml's own <br> is display: inline-block (master_css.h); whatever display the book gives it but none.
    if(css().get_display() != display_none && css().get_display() != display_inline_block)
    {
        css_w().set_display(display_inline_block);
    }
}
