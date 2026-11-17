#ifndef LITEHTML_EL_TEXT_H
#define LITEHTML_EL_TEXT_H

#include "element.h"
#include "document.h"
#include "cjk_rules.h"

namespace litehtml
{
    class text_breaker;

    /// A text node's whole text, which its pieces (words and spaces, el_text) are cut from. Shaped once per font by
    /// the container (document_container::text_advances): a piece's width is then its share of the shaped run, with
    /// the font's kerning between pieces, as a browser shapes a text node and breaks the shaped result into lines.
    struct shaped_text
    {
        std::string text;
        /// Per font: the advance before each byte (empty: the container does not shape runs).
        std::vector<std::pair<uint_ptr, std::vector<pixel_t>>> advances;
    };

    /// How far text-align: justify moves the boundary at byte `at` of a piece's text, which has `gap` more at each of
    /// its justifiable gaps (cjk::justifiable_gaps, render_text::justify_gap). A boundary at a gap is two places: the end
    /// of the character before it, and the start of the one after it (`after_gap`).
    pixel_t justify_shift(const std::string& text, size_t at, pixel_t gap, bool after_gap = true);

    /// Punctuation compression of one character of a piece (cjk::compression): its byte in the piece's text, and the
    /// pixels of its advance it gives up before its glyph and after it.
    struct char_trim
    {
        size_t  at;
        pixel_t start, end;
    };

    /// How much the characters of a piece before byte `at` give up (render_text::trims): the boundary at `at` is that
    /// much nearer the piece's start.
    pixel_t trim_shift(const std::vector<char_trim>& trims, size_t at);

    class el_text : public element
    {
      protected:
        std::string m_text;
        std::string m_transformed_text;
        size        m_size;
        bool        m_use_transformed;
        bool        m_draw_spaces;
        uint_ptr    m_measured_font = 0;
        int         m_measured_transform = -1;
        std::shared_ptr<text_breaker> m_breaker;
        mutable int8_t m_strong_direction = 2;  // 2: not looked at yet
        // Where the text came from: the source text node's byte position in the parsed string, and this text's
        // UTF-16 offset in that node's (decoded) text; UINT32_MAX when it was not parsed from a string.
        uint32_t m_src_pos = UINT32_MAX;
        uint32_t m_src_utf16 = 0;
        // The run this piece was cut from and its bytes in it (none: measured alone).
        std::shared_ptr<shaped_text> m_run;
        uint32_t m_run_begin = 0, m_run_end = 0;
        // Its width shaped alone less its share of the run: what it takes more when it ends a line (Blink shapes a
        // line's end again after the break, without the kerning with what went to the next line).
        pixel_t m_end_extra = 0_px;

      public:
        el_text(const char* text, const document::ptr& doc);

        void get_text(std::string& text) const override;
        void set_text(const std::string& text);
        void set_source(uint32_t pos, uint32_t utf16)
        {
            m_src_pos   = pos;
            m_src_utf16 = utf16;
        }
        /// The run this piece is bytes [begin, end) of (set by the parser when the pieces are the node's text in order).
        void set_run(const std::shared_ptr<shaped_text>& run, uint32_t begin, uint32_t end)
        {
            m_run       = run;
            m_run_begin = begin;
            m_run_end   = end;
            m_measured_font = 0;
        }
        /// What this piece takes more than its width when it ends a line: its kerning with the next piece, and a hyphen
        /// after a soft hyphen.
        pixel_t line_end_extra() const { return m_end_extra; }
        /// Whether it ends with a soft hyphen (U+00AD): a break opportunity, drawn as a hyphen when the line breaks there.
        bool ends_with_soft_hyphen() const
        {
            const std::string& shown = display_text();
            return shown.size() >= 2 && shown.compare(shown.size() - 2, 2, "\xC2\xAD") == 0;
        }
        /// What a line broken at its soft hyphen draws: a hyphen-minus for it (a glyph every Latin font has, where
        /// Blink takes U+2010 when the font has one).
        std::string hyphenated() const { return display_text().substr(0, display_text().size() - 2) + "-"; }
        /// How far the first `bytes` of `shown` reach along a horizontal line: when `shown` is its whole text, its
        /// share of the shaped run, as its width is (with the kerning inside it, as the piece is drawn whole); otherwise
        /// (a fragment, the hyphenated end) measured alone.
        pixel_t advance_to(const std::string& shown, size_t bytes) const;
        /// Punctuation compression of `shown` (its text) between `before` and `after`, the characters next to it in its
        /// line's flow (0: none), its first and last characters at `first` and `last` (cjk::line_position): each trimmed
        /// character, with its share of its advance (an em, upright in vertical text).
        std::vector<char_trim> compression(const std::string& shown, char32_t before, char32_t after, uint8_t first,
                                           uint8_t last) const;
        uint32_t source_pos() const { return m_src_pos; }
        uint32_t source_utf16() const { return m_src_utf16; }
        void compute_styles(bool recursive) override;
        const std::string& display_text() const { return m_use_transformed ? m_transformed_text : m_text; }
        std::shared_ptr<text_breaker> breaker();
        /// The direction of the text's first strong character: 1 right to left, -1 left to right, 0 none
        /// (digits, punctuation, spaces, symbols).
        int strong_direction() const;
        bool is_text() const override
        {
            return true;
        }

        void        draw(uint_ptr hdc, pixel_t x, pixel_t y, const position* clip,
                         const std::shared_ptr<render_item>& ri) override;
        std::string dump_get_name() override;

        std::vector<std::tuple<std::string, std::string>> dump_get_attrs() override;

      protected:
        void get_content_size(size& sz, pixel_t max_width) override;
    };
} // namespace litehtml

#endif // LITEHTML_EL_TEXT_H
