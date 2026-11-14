#ifndef LITEHTML_VERTICAL_H
#define LITEHTML_VERTICAL_H

#include "css_effects.h"

namespace litehtml
{
    /// UAX #50 (Vertical_Orientation), simplified: true for the characters text-orientation: mixed sets upright in
    /// vertical text (U, Tu and Tr: CJK, kana, Hangul, fullwidth forms, CJK punctuation and brackets, most symbols);
    /// false for the ones it turns sideways (R: Latin, digits, ASCII punctuation, dashes). Tr characters are drawn
    /// upright with the font's vert forms, which every CJK font has for them.
    bool upright_in_vertical(char32_t cp);

    /// A vertical-rl layout is a horizontal one turned 90° clockwise: logical (x, y) is the page's (right - y, x).
    inline transform_matrix vertical_rl_matrix(float right, float top)
    {
        return {0, 1, -1, 0, right, top};
    }

    /// Turns what is drawn about (cx, cy) by 90° counter-clockwise, undoing vertical_rl_matrix for an upright glyph.
    inline transform_matrix upright_matrix(float cx, float cy)
    {
        return {0, -1, 1, 0, cx - cy, cx + cy};
    }
} // namespace litehtml

#endif // LITEHTML_VERTICAL_H
