#ifndef LITEHTML_EL_BREAK_H
#define LITEHTML_EL_BREAK_H

#include "html_tag.h"

namespace litehtml
{
    class el_break : public html_tag
    {
      public:
        explicit el_break(const std::shared_ptr<litehtml::document>& doc);

        bool is_break() const override;
        /// A <br> is a line break whatever its display (but none), as in Blink, which makes a <br> a line break box
        /// for any other display (here: litehtml's own <br>, display: inline-block): calibre's <br class="calibre1"/> with .calibre1 { display: block } would otherwise
        /// be an empty block, losing a chapter's leading empty line and every blank line.
        void compute_styles(bool recursive = true) override;
    };
} // namespace litehtml

#endif // LITEHTML_EL_BREAK_H
