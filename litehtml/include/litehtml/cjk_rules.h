#ifndef LH_CJK_RULES_H
#define LH_CJK_RULES_H

#include <cstdint>

// The CJK line-composition rule set: what each character is to line breaking (and, for the steps to come, to
// justification and punctuation compression). Its table is generated from stone_engine's (tools/cjk_rules); callers
// ask here and never name stone.
namespace litehtml::cjk
{
    // A character's classes, one bit each; a character may have several, most have none.
    enum class_bit : uint16_t
    {
        not_starting    = 1 << 0, // never starts a line: closing brackets, 、。, small kana, ー, ・：; ... (JLREQ level 4)
        not_ending      = 1 << 1, // never ends a line: opening brackets and quotes
        inseparable     = 1 << 2, // no break between two of them: …… ‥‥ —— ―― ──
        opening         = 1 << 3, // an opening bracket, its glyph in the em's second half
        closing         = 1 << 4, // a closing bracket, its glyph in the em's first half
        middle_dot      = 1 << 5, // ・：；, the glyph in the em's middle
        full_stop_comma = 1 << 6, // 、。, the glyph in the em's first half
        hanging         = 1 << 7, // may hang past a line's end
    };
    using classes = uint16_t;

    // The classes of a code point (0 for most).
    classes classify(char32_t c);

    // Whether no line may break between before and after: before never ends a line, after never starts one, or both
    // are inseparable.
    bool no_break_between(char32_t before, char32_t after);
} // namespace litehtml::cjk

#endif // LH_CJK_RULES_H
