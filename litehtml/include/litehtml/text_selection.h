#ifndef LITEHTML_TEXT_SELECTION_H
#define LITEHTML_TEXT_SELECTION_H

#include "document.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace litehtml
{
    class el_text;

    /// A place in the text: a text element and a byte offset into its displayed text.
    struct text_position
    {
        std::shared_ptr<el_text> text;
        size_t                   offset = 0;
        bool valid() const { return text != nullptr; }
    };

    /// A piece of text as laid out, in document order: a word, a space, or the part of one on a line.
    struct text_run
    {
        std::shared_ptr<el_text> text;
        size_t                   offset = 0;  // in the element's displayed text
        std::string              shown;
        position                 box;         // laid-out coordinates (document::vertical() turns them onto the page)
        int                      block = 0;   // the block container it flows in
        bool                     annotation = false;  // inside a ruby annotation (<rt>)
        pixel_t                  justify_gap = 0_px;  // text-align: justify's at each of its gaps (render_text::justify_gap)
    };

    /// Text selection over a laid-out document, horizontal or vertical-rl (a reader's selection, not a widget's:
    /// TwidgetCore keeps its own for user-select in widgets). Points and rectangles are page coordinates, as
    /// document::draw paints at (0, 0). Ruby annotations are left out: a point never lands in one, the
    /// highlight and the text skip them, as Chromium copies base text.
    class text_selection
    {
      public:
        explicit text_selection(document& doc);

        const std::vector<text_run>& runs() const { return m_runs; }
        /// The place nearest the point: the closest line (a column in vertical text), the closest character
        /// boundary along it.
        text_position position_at(pixel_t x, pixel_t y) const;
        /// One rectangle per line the range [a, b) covers (either order), on the page.
        std::vector<position> rects(const text_position& a, const text_position& b) const;
        /// The range's base text; a line break between blocks.
        std::string text(const text_position& a, const text_position& b) const;
        /// The block `at` is in, from its first character to its last.
        bool paragraph(const text_position& at, text_position& start, text_position& end) const;
        /// The sentence `at` is in: after the previous 。！？.!? (and the closing brackets after it) up to and
        /// with the next one and its closing brackets, inside the paragraph.
        bool sentence(const text_position& at, text_position& start, text_position& end) const;
        /// Laid-out coordinates to the page's, and back.
        position to_page(const position& box) const;
        void     to_layout(pixel_t x, pixel_t y, pixel_t& lx, pixel_t& ly) const;

      private:
        document&             m_doc;
        std::vector<text_run> m_runs;
        /// The first run of each text element (its runs are consecutive).
        std::unordered_map<const el_text*, size_t> m_first_run;
        /// The run and the offset in it; false when the position is in no run.
        bool    locate(const text_position& p, size_t& run, size_t& offset) const;
        /// How far along the run the character boundary at `bytes` is: at a gap justify widened, the end of the
        /// character before it, or (after_gap) the start of the one after it.
        pixel_t advance(const text_run& run, size_t bytes, bool after_gap) const;
        bool    ordered(const text_position& a, const text_position& b, size_t& r0, size_t& o0, size_t& r1, size_t& o1) const;
    };
} // namespace litehtml

#endif // LITEHTML_TEXT_SELECTION_H
