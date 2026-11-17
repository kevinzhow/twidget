#ifndef LH_CJK_RULES_H
#define LH_CJK_RULES_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The CJK line-composition rule set: what each character is to line breaking, to justification (and, for the steps to
// come, to punctuation compression). Its tables are generated from stone_engine's (tools/cjk_rules); callers ask here
// and never name stone.
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

    // The script a character is set in: Japanese (kana, kanji, CJK and full-width punctuation, — ― … ‥ ※ and the
    // like), emoji, or else Latin (letters, digits and symbols of every other script).
    enum class script : uint8_t
    {
        latin,
        japanese,
        emoji,
    };
    script script_of(char32_t c);

    // Whether text-align: justify may widen the gap between two adjacent characters: at a space, between Japanese
    // characters, and where Latin meets Japanese; not between two Latin characters (inside a word or number), between
    // two inseparable ones (…… ―― ──), nor inside a cluster (before a combining mark or a variation selector).
    bool justifiable_gap(char32_t before, char32_t after);

    // The gaps between the characters of UTF-8 text that justify may widen: the byte offset of the character after
    // each, in order.
    std::vector<size_t> justifiable_gaps(const std::string& text);
} // namespace litehtml::cjk

#endif // LH_CJK_RULES_H
