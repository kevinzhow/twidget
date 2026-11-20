#ifndef LITEHTML_LINE_BOX_H
#define LITEHTML_LINE_BOX_H

#include <memory>
#include "css_properties.h"
#include "types.h"

namespace litehtml
{
    class render_item;
    class render_text;

    struct line_context
    {
        pixel_t calculatedTop;
        pixel_t top;
        pixel_t left;
        pixel_t right;

        pixel_t width() const
        {
            return right - left;
        }
        void fix_top()
        {
            calculatedTop = top;
        }
        line_context() :
            calculatedTop(0),
            top(0),
            left(0),
            right(0)
        {
        }
    };

    class line_box_item
    {
      public:
        enum element_type
        {
            type_text_part,
            type_inline_start,
            type_inline_continue,
            type_inline_end
        };

      protected:
        std::shared_ptr<render_item> m_element;
        pixel_t                      m_rendered_min_width;
        pixel_t                      m_items_top;
        pixel_t                      m_items_bottom;

      public:
        explicit line_box_item(const std::shared_ptr<render_item>& element) :
            m_element(element)
        {
        }
        line_box_item(const line_box_item& el) = default;
        line_box_item(line_box_item&&)         = default;
        virtual ~line_box_item();

        virtual pixel_t                     height() const;
        const std::shared_ptr<render_item>& get_el() const
        {
            return m_element;
        }
        virtual position&    pos();
        virtual void         place_to(pixel_t x, pixel_t y);
        virtual pixel_t      width() const;
        virtual pixel_t      top() const;
        virtual pixel_t      bottom() const;
        virtual pixel_t      right() const;
        virtual pixel_t      left() const;
        virtual element_type get_type() const
        {
            return type_text_part;
        }
        virtual pixel_t get_rendered_min_width() const
        {
            return m_rendered_min_width;
        }
        virtual pixel_t get_rendered_natural_width() const
        {
            return get_rendered_min_width();
        }
        virtual void set_rendered_min_width(pixel_t min_width)
        {
            m_rendered_min_width = min_width;
        }
        virtual void y_shift(pixel_t shift);

        void reset_items_height()
        {
            m_items_top = m_items_bottom = 0_px;
        }
        void add_item_height(pixel_t item_top, pixel_t item_bottom)
        {
            m_items_top    = std::min(m_items_top, item_top);
            m_items_bottom = std::max(m_items_bottom, item_bottom);
        }
        pixel_t get_items_top() const
        {
            return m_items_top;
        }
        pixel_t get_items_bottom() const
        {
            return m_items_bottom;
        }
    };

    class lbi_start : public line_box_item
    {
      protected:
        position m_pos;

      public:
        explicit lbi_start(const std::shared_ptr<render_item>& element);
        ~lbi_start() override;

        void      place_to(pixel_t x, pixel_t y) override;
        pixel_t   height() const override;
        pixel_t   width() const override;
        position& pos() override
        {
            return m_pos;
        }
        pixel_t      top() const override;
        pixel_t      bottom() const override;
        pixel_t      right() const override;
        pixel_t      left() const override;
        element_type get_type() const override
        {
            return type_inline_start;
        }
        pixel_t get_rendered_min_width() const override
        {
            return width();
        }
    };

    class lbi_end : public lbi_start
    {
      public:
        explicit lbi_end(const std::shared_ptr<render_item>& element);
        ~lbi_end() override;

        void         place_to(pixel_t x, pixel_t y) override;
        pixel_t      right() const override;
        pixel_t      left() const override;
        element_type get_type() const override
        {
            return type_inline_end;
        }
        void y_shift(pixel_t) override {}
    };

    class lbi_continue : public lbi_start
    {
      public:
        explicit lbi_continue(const std::shared_ptr<render_item>& element);
        ~lbi_continue() override;

        void         place_to(pixel_t x, pixel_t y) override;
        pixel_t      right() const override;
        pixel_t      left() const override;
        pixel_t      width() const override;
        element_type get_type() const override
        {
            return type_inline_continue;
        }
    };

    /// Whether there is no break opportunity between two adjacent pieces of a line's text, `before` and `after` (el_text
    /// or el_space; CSS Text 3 §5): the nearest common ancestor of the two has white-space nowrap or pre (§5.1); both
    /// are letters under word-break: keep-all (§5.2); or, with `kinsoku` (pieces of different texts: one text's pieces
    /// are where the container's split_text broke it), the CJK rule set forbids a break (cjk::no_break_between).
    bool no_break_opportunity(const std::shared_ptr<element>& before, const std::shared_ptr<element>& after, bool kinsoku);
    /// Whether one of two pieces next to each other is in a box of vertical-align super or sub (a script, 添え字) that
    /// the other is not in, under script_breaking::keep: they are set solid (no quarter em between Japanese and Latin).
    bool script_boundary(const std::shared_ptr<element>& before, const std::shared_ptr<element>& after);
    /// Whether `before` and `after` are pieces of one text: adjacent children of one element.
    bool same_text(const std::shared_ptr<element>& before, const std::shared_ptr<element>& after);

    class line_box
    {
        struct va_context
        {
            pixel_t        line_height;
            pixel_t        baseline;
            font_metrics   fm;
            line_box_item* start_lbi = nullptr;
        };

        pixel_t           m_top;
        pixel_t           m_left;
        pixel_t           m_right;
        pixel_t           m_height;
        pixel_t           m_width;
        css_line_height_t m_default_line_height;
        font_metrics      m_font_metrics;
        pixel_t           m_baseline;
        text_align        m_text_align;
        text_align_last   m_text_align_last = text_align_last_auto;
        bool              m_rtl;
        rendered_width    m_rendered_width;
        bool              m_paragraph_start = false;  // set_paragraph_start
        pixel_t           m_bracket_space   = 0;      // still to put before a paragraph's opening bracket
        bool              m_force_hold          = false;  // JLReq 3.5.4 pull-in pass
        bool              m_overfull            = false;  // remained overfull after squeeze()
        bool              m_finished_with_break = false;
        pixel_t           m_pre_finish_width    = 0_px;

        std::list<std::unique_ptr<line_box_item>> m_items;

      public:
        line_box(pixel_t top, pixel_t left, pixel_t right, const css_line_height_t& line_height, const font_metrics& fm,
                 text_align align, bool rtl = false, text_align_last align_last = text_align_last_auto) :
            m_top(top),
            m_left(left),
            m_right(right),
            m_height(0),
            m_width(0),
            m_default_line_height(line_height),
            m_font_metrics(fm),
            m_baseline(0),
            m_text_align(align),
            m_text_align_last(align_last),
            m_rtl(rtl)
        {
        }

        pixel_t bottom() const
        {
            return m_top + height();
        }
        const std::list<std::unique_ptr<line_box_item>>& items() const
        {
            return m_items;
        }
        pixel_t top() const
        {
            return m_top;
        }
        pixel_t right() const
        {
            return m_left + width();
        }
        pixel_t left() const
        {
            return m_left;
        }
        pixel_t height() const
        {
            return m_height;
        }
        pixel_t width() const
        {
            return m_width;
        }
        pixel_t line_right() const
        {
            return m_right;
        }
        rendered_width get_rendered_width() const
        {
            return m_rendered_width;
        }

        /// The line starts a paragraph (a block's first line, or the line after a <br>) of a book that indents its
        /// paragraphs, and has less than an em of indent itself: an opening bracket that starts it keeps its full width,
        /// after `bracket_space` more (JLReq 3.1.5 ①: 改行行頭 全角アキ; a wrapped line's opening bracket stays 天ツキ).
        void    set_paragraph_start(pixel_t bracket_space)
        {
            m_paragraph_start = true;
            m_bracket_space   = bracket_space;
        }
        void    set_force_hold(bool on) { m_force_hold = on; }
        bool    overfull() const { return m_overfull; }
        bool    finished_with_break() const { return m_finished_with_break; }
        pixel_t pre_finish_width() const { return m_pre_finish_width; }
        void    add_item(std::unique_ptr<line_box_item> item);
        bool    can_hold(const std::unique_ptr<line_box_item>& item, white_space ws, pixel_t trailing = 0_px) const;
        /// Where the line's first character is, for compression: line_start, and paragraph_start after
        /// set_paragraph_start.
        uint8_t start_position() const;
        static bool starts_with_opening(const std::shared_ptr<render_text>& part);
        /// The last item placed that shows something (not an inline element's start or end, nor a collapsed space).
        const line_box_item* last_content_item() const;
        /// The content item immediately before `target` on this line, or nullptr if `target` is the first content item.
        const line_box_item* prev_content_item(const line_box_item* target) const;
        struct ruby_plan
        {
            pixel_t              overlap        = 0_px;
            pixel_t              prev_shift     = 0_px;
            pixel_t              prev_move_left = 0_px;
            pixel_t              prev_ann_shift = 0_px;
            pixel_t              item_ann_shift = 0_px;
            pixel_t              item_ann_used  = 0_px;
            const line_box_item* prev_before    = nullptr;
        };
        /// Plans ruby overhang and asymmetric base/annotation shifts when placing `item` after the line's last content
        /// item (JLReq 3.3.5, 3.3.7, 3.3.8). Side-effect free so can_hold, squeeze_room, and add_item stay in sync.
        ruby_plan            plan_ruby(const line_box_item* item) const;
        /// Total width saved when placing `item` after the last content item (overlap plus any leftward shift of prev).
        pixel_t ruby_overlap(const line_box_item* item) const;
        /// How far a ruby (its ruby character `ruby_em`) may overhang `item` next to its base, `item` before it or after
        /// it (JLReq 3.3.8): a kana up to one ruby character (rule 2); with ruby_overhang::punctuation also the half em
        /// of 」、。 before it or 「 after it (rule 3), …… and ―― (rule 4), a middle dot (rule 5: its quarter and half a
        /// ruby character), and the glyph of 「 before it or 」、。 after it (rule 6), each up to one ruby character.
        /// -1: not at all (a kanji, another ruby, Latin text).
        pixel_t overhang_room(const line_box_item* item, bool before_ruby, pixel_t ruby_em) const;
        /// How far a ruby that does not fit the line with ruby_overlap overlaps the kana before it to fit ending the
        /// line (JLReq 3.3.8 line-edge b: its whole excess, up to one ruby character); -1 when that does not apply.
        pixel_t ruby_end_overlap(const line_box_item* item, pixel_t extra) const;
        /// How much a line that `next` ends (when `ends_line` is true) or continues (when `ends_line` is false because
        /// trailing inseparable items follow `next` on the same line) may squeeze (JLReq 3.8.3).
        pixel_t squeeze_room(const line_box_item* next, bool ends_line = true) const;
        bool    is_empty() const;
        pixel_t baseline() const;
        pixel_t top_margin() const;
        pixel_t bottom_margin() const;
        void    y_shift(pixel_t shift);

        std::list<std::unique_ptr<line_box_item>>  finish(bool                            last_box,
                                                          const containing_block_context& containing_block_size);
        std::list<std::unique_ptr<line_box_item>>  new_width(pixel_t left, pixel_t right);
        std::shared_ptr<render_item>               get_last_text_part() const;
        std::shared_ptr<render_item>               get_first_text_part() const;
        std::list<std::unique_ptr<line_box_item>>& items()
        {
            return m_items;
        }

      private:
        /// Visual order: a right to left line mirrored, and runs of the other direction turned back.
        void           reorder_bidi();
        bool           have_last_space() const;
        /// Whether no text (nor inline box) comes before `at` on the line: only inline starts and ends.
        bool           first_text_part(std::list<std::unique_ptr<line_box_item>>::const_iterator at) const;
        bool           is_break_only() const;
        /// Squeezes a line `over` too long in JLReq 3.8.3's order (Western word spaces first, then a non-wrapping
        /// line's trailing closing bracket or 、。, then middle dots, mid-line brackets/commas, and 和欧間), returning
        /// any remaining overflow.
        pixel_t        squeeze(pixel_t over, bool wraps);
        static pixel_t calc_va_baseline(const va_context& current, vertical_align va, const font_metrics& new_font,
                                        pixel_t top, pixel_t bottom, bool vertical = false);
    };
} // namespace litehtml

#endif // LITEHTML_LINE_BOX_H
