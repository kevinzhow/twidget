#ifndef LITEHTML_RENDER_RUBY_H
#define LITEHTML_RENDER_RUBY_H

#include "render_block_context.h"

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
        explicit render_item_ruby_pair(std::shared_ptr<element> src_el) :
            render_item_block_context(std::move(src_el))
        {
        }
        std::shared_ptr<render_item> clone() override
        {
            auto ret            = std::make_shared<render_item_ruby_pair>(src_el());
            ret->has_annotation = has_annotation;
            return ret;
        }
    };
} // namespace litehtml

#endif // LITEHTML_RENDER_RUBY_H
