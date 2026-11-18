#ifndef LH_CJK_RULES_H
#define LH_CJK_RULES_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The CJK line-composition rule set: what each character is to line breaking, to justification and to punctuation
// compression. Its tables are generated from stone_engine's (tools/cjk_rules); callers ask here and never name stone.
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

    // Where a character is on its line, for compression: a line's first character, the last character of a line that
    // wraps (not one before a forced break, nor on a paragraph's last line), both, or neither. paragraph_start (with
    // line_start) is the first character of a paragraph whose opening bracket keeps its first half (JLReq 3.1.5 ①: the
    // paragraph's line head is 全角アキ, line_box::set_paragraph_start).
    enum line_position : uint8_t
    {
        line_middle     = 0,
        line_start      = 1 << 0,
        line_end        = 1 << 1,
        paragraph_start = 1 << 2,
    };

    // What punctuation compression takes from a character: the part of its advance (of its em) before its glyph and
    // after it, along the line.
    struct trim
    {
        float start = 0;
        float end   = 0;
    };

    // Punctuation compression of c between prev and next (0: none, as at a paragraph's start) at `at` (line_position
    // bits): an opening bracket at a line's start (not at a paragraph_start) or after another punctuation mark gives up
    // its first half; a closing
    // bracket or 、。 before a closing bracket, 、。 or a middle dot gives up its second half, and so does one ending a
    // line that wraps. Nothing else is trimmed (・：； keep their ems).
    trim compression(char32_t prev, char32_t c, char32_t next, uint8_t at);

    // Bytes [from, to) of a text.
    struct span
    {
        size_t from = 0, to = 0;
        bool   operator==(const span& o) const { return from == o.from && to == o.to; }
    };

    // Tate-chu-yoko (縦中横): the tokens of text[from, to) that vertical text sets upright, side by side in one em of
    // the column, in order. A token of one or two half-width (ASCII) digits, as a word boundary ends it (not 123, 1.5,
    // A12, nor 1２ next to a full-width digit); and one of the overlay's tokens (!? !! ?! ??, ‼ ⁉ ⁈ ⁇) with no Latin
    // character next to it. The characters of text around [from, to) decide where a token ends; one that runs past
    // either end is not returned.
    std::vector<span> tate_chu_yoko(const std::string& text, size_t from, size_t to);
} // namespace litehtml::cjk

#endif // LH_CJK_RULES_H
