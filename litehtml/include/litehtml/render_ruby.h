#ifndef LITEHTML_RENDER_RUBY_H
#define LITEHTML_RENDER_RUBY_H

#include "render_block_context.h"
#include "render_inline_context.h"

namespace litehtml
{
    /**
     * <ruby>: an atomic inline (the master CSS makes it inline-block) whose content is one box per base and
     * annotation pair: the annotation (<rt>, a block) over the base (the inline content before it), each centred
     * in the pair, the pair as wide as the wider of the two. Lines may break between pairs. <rp> is display: none.
     * A prototype of CSS Ruby 1: no ruby-align (space-around), ruby-position, ruby-merge or overhang.
     */
    class render_item_ruby : public render_item_block
    {
      public:
        explicit render_item_ruby(std::shared_ptr<element> src_el) :
            render_item_block(std::move(src_el))
        {
        }
        std::shared_ptr<render_item> clone() override
        {
            return std::make_shared<render_item_ruby>(src_el());
        }
        std::shared_ptr<render_item> init() override;
    };

    /// One base and annotation pair: the annotation first, the base under it; the pair's box is the base's, the
    /// annotation hangs over it.
    class render_item_ruby_pair : public render_item_block_context
    {
      protected:
        rendered_width _render_content(pixel_t x, pixel_t y, bool second_pass,
                                       const containing_block_context& self_size, formatting_context* fmt_ctx) override;

      public:
        bool has_annotation = false;
        /// How far the annotation's em box reaches above the pair's box (its base's).
        pixel_t annotation_overflow = 0_px;
        /// Where its base's text starts and ends in the pair (from the pair's start, as laid out: centred, or spread by
        /// ruby-align), and one ruby character (the annotation's font size): an annotation longer than its base
        /// overhangs the text next to the pair by what lies outside [base_start, base_end] (line_box, JLReq 3.3.8).
        pixel_t base_start = 0_px, base_end = 0_px, ruby_em = 0_px;
        /// Where its annotation's text starts and ends in the pair: a reading shorter than its base leaves the rest of
        /// the pair's width free above the base, which a longer reading of the same word may overhang (JLReq 3.3.7).
        pixel_t ann_start = 0_px, ann_end = 0_px;
        /// One of the pairs of a word (熟語ルビ, a <ruby> of several pairs: render_item_ruby_word).
        bool word_member = false;
        /// Whether it is a base unit of a word in the line, a line item of its own before which the line may break
        /// (a word member, and the document's ruby_breaking is per_base).
        bool in_word() const;
        /// Whether its base's text is Japanese (cjk::script_of its first character): JLReq's overhang and line-edge
        /// rules apply to it; a ruby over Latin text stays as laid out.
        bool japanese_base = false;
        /// How far its base is moved along the line from where it was laid out (align_base).
        pixel_t base_shift = 0_px;
        /// How much of the text before it the pair overlaps (line_box::add_item).
        pixel_t overlap_before = 0_px;
        /// Moves its base `shift` along the line from where it was laid out: at a line's head the base starts with the
        /// annotation, at a line's end it ends with it (JLReq 3.3.8 line-edge a, b).
        void align_base(pixel_t shift);
        explicit render_item_ruby_pair(std::shared_ptr<element> src_el) :
            render_item_block_context(std::move(src_el))
        {
        }
        std::shared_ptr<render_item> clone() override
        {
            auto ret            = std::make_shared<render_item_ruby_pair>(src_el());
            ret->has_annotation = has_annotation;
            ret->word_member    = word_member;
            return ret;
        }
    };

    /// A <ruby> of several pairs (熟語ルビ): with ruby_breaking::per_base an inline, like a span, whose pairs are
    /// inline-blocks of the line around it (iterators.cpp go_inside_inline goes into it), so that the line may break
    /// between them; with whole, an atomic inline-block, the inline formatting context of its pairs.
    class render_item_ruby_word : public render_item_inline_context
    {
      public:
        explicit render_item_ruby_word(std::shared_ptr<element> src_el) :
            render_item_inline_context(std::move(src_el))
        {
        }
        /// Whether its pairs are items of the line around it (ruby_breaking::per_base).
        bool split() const;
        // Split, it is an inline element of the line around it, as a <span> (render_item_inline): its baselines are
        // its font's, not those of the lines it has when laid out whole (JLReq 3.3.2: no extra line pitch).
        pixel_t get_first_baseline() override
        {
            return split() ? span_baseline() : render_item_inline_context::get_first_baseline();
        }
        pixel_t get_last_baseline() override
        {
            return split() ? span_baseline() : render_item_inline_context::get_last_baseline();
        }
        // Split, its pairs are items of the lines around it and move with them; it stays put, as they are drawn
        // from it (a line's start item moving it too moved every pair of the word again, on all its lines).
        void y_shift(pixel_t delta) override
        {
            if(!split())
                render_item_inline_context::y_shift(delta);
        }
        pixel_t span_baseline() const
        {
            return src_el()->css().get_font_metrics().height - src_el()->css().get_font_metrics().base_line();
        }
        std::shared_ptr<render_item> clone() override
        {
            return std::make_shared<render_item_ruby_word>(src_el());
        }
    };
} // namespace litehtml

#endif // LITEHTML_RENDER_RUBY_H
