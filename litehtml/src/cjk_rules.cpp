#include "cjk_rules.h"

#include <algorithm>
#include <iterator>

namespace litehtml::cjk
{
    namespace
    {
        struct entry
        {
            char32_t code_point;
            classes  is;
        };
        constexpr entry table[] = {
#include "cjk_rules_table.inc"
        };

        struct script_range
        {
            char32_t first, last;
            script   is;
        };
        constexpr script_range scripts[] = {
#include "cjk_scripts_table.inc"
        };

        // stone's isSpaceChar: JavaScript's \s less the line breaks.
        bool is_space(char32_t c)
        {
            return c == '\t' || c == ' ' || c == 0x00A0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F ||
                   c == 0x205F || c == 0x3000 || c == 0xFEFF;
        }

        // A character that joins the one before it into one grapheme cluster (stone's runs are clusters): combining
        // marks, the combining kana voicing marks, variation selectors (a kanji's IVS), joiners, emoji modifiers and tags.
        bool extends_cluster(char32_t c)
        {
            return (c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) ||
                   (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F) || c == 0x3099 || c == 0x309A ||
                   (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF) || c == 0x200C || c == 0x200D ||
                   (c >= 0x1F3FB && c <= 0x1F3FF) || (c >= 0xE0020 && c <= 0xE007F);
        }
    } // namespace

    classes classify(char32_t c)
    {
        auto it = std::lower_bound(std::begin(table), std::end(table), c,
                                   [](const entry& e, char32_t x) { return e.code_point < x; });
        return it != std::end(table) && it->code_point == c ? it->is : 0;
    }

    bool no_break_between(char32_t before, char32_t after)
    {
        const classes a = classify(before), b = classify(after);
        return (a & not_ending) || (b & not_starting) || (a & b & inseparable);
    }

    script script_of(char32_t c)
    {
        auto it = std::lower_bound(std::begin(scripts), std::end(scripts), c,
                                   [](const script_range& r, char32_t x) { return r.last < x; });
        return it != std::end(scripts) && it->first <= c ? it->is : script::latin;
    }

    // stone_engine_js layout.ts isJustifiableGap (manifest.json "mirrored" pins the text this follows).
    bool justifiable_gap(char32_t before, char32_t after)
    {
        if(extends_cluster(after) || before == 0x200D)
            return false;
        if(is_space(before) || is_space(after))
            return true;
        if(classify(before) & classify(after) & inseparable)
            return false;
        return script_of(before) != script::latin || script_of(after) != script::latin;
    }

    // stone_engine_js layout.ts punctuationTrim in its "stone" mode, halvesOpeningAfterPunctuation,
    // halvesClosingBeforePunctuation and the line-end trim of goNextLineLrTb / goNextLineTbRl (manifest.json "mirrored").
    // stone's "firstHalf" marks are closing | full_stop_comma, "secondHalf" opening, "quarter" middle_dot.
    trim compression(char32_t prev, char32_t c, char32_t next, uint8_t at)
    {
        constexpr classes first_half  = closing | full_stop_comma;
        constexpr classes punctuation = opening | first_half | middle_dot;
        const classes     is          = classify(c);
        if((is & opening) && ((at & line_start) || (classify(prev) & punctuation)))
            return {0.5f, 0};
        if((is & first_half) && ((classify(next) & (first_half | middle_dot)) || (at & line_end)))
            return {0, 0.5f};
        return {};
    }

    std::vector<size_t> justifiable_gaps(const std::string& text)
    {
        std::vector<size_t> gaps;
        char32_t            before = 0;
        for(size_t i = 0; i < text.size();)
        {
            const unsigned char c0 = text[i];
            size_t n = c0 < 0x80 ? 1 : (c0 >> 5) == 6 ? 2 : (c0 >> 4) == 14 ? 3 : 4;
            n        = std::min(n, text.size() - i);
            char32_t c = n == 1 ? c0 : n == 2 ? c0 & 0x1F : n == 3 ? c0 & 0x0F : c0 & 0x07;
            for(size_t k = 1; k < n; k++)
                c = (c << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
            if(i > 0 && justifiable_gap(before, c))
                gaps.push_back(i);
            before = c;
            i     += n;
        }
        return gaps;
    }
} // namespace litehtml::cjk
