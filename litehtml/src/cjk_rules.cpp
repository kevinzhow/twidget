#include "cjk_rules.h"

#include <algorithm>
#include <string_view>
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

        // The tokens the overlay sets as tate-chu-yoko (UTF-8), besides stone's numbers.
        constexpr const char* tate_chu_yoko_tokens[] = {
#include "cjk_tate_chu_yoko_table.inc"
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

        // The code point at text[i], and its length in bytes.
        char32_t decode(const std::string& text, size_t i, size_t& n)
        {
            const unsigned char c0 = text[i];
            n = c0 < 0x80 ? 1 : (c0 >> 5) == 6 ? 2 : (c0 >> 4) == 14 ? 3 : 4;
            n = std::min(n, text.size() - i);
            char32_t c = n == 1 ? c0 : n == 2 ? c0 & 0x1F : n == 3 ? c0 & 0x0F : c0 & 0x07;
            for(size_t k = 1; k < n; k++)
                c = (c << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
            return c;
        }

        // The code point ending at byte i (0 at the text's start), and where it starts.
        char32_t decode_before(const std::string& text, size_t i, size_t& at)
        {
            at = i;
            if(i == 0)
                return 0;
            do
                at--;
            while(at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80);
            size_t n;
            return decode(text, at, n);
        }

        bool ascii_digit(char32_t c) { return c >= '0' && c <= '9'; }

        // stone's tokens are Intl.Segmenter("ja") words (parser.ts splitWords): around a number, UAX #29's word
        // boundaries. A number joins the letters and digits next to it (WB9-WB10: ALetter, Numeric; ExtendNumLet _),
        // of the scripts a Japanese book mixes in: Latin, Greek, Cyrillic, Roman numerals, full-width letters and digits.
        // Kana and kanji do not join it.
        bool word_char(char32_t c)
        {
            return (c < 0x80 && (ascii_digit(c) || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || c == '_')) || c == 0xAA ||
                   c == 0xB5 || c == 0xBA || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) ||
                   (c >= 0x370 && c <= 0x52F && c != 0x37E && c != 0x387) || (c >= 0x2160 && c <= 0x2188) ||
                   (c >= 0xFF10 && c <= 0xFF19) || (c >= 0xFF21 && c <= 0xFF3A) || (c >= 0xFF41 && c <= 0xFF5A);
        }

        bool numeric(char32_t c) { return ascii_digit(c) || (c >= 0xFF10 && c <= 0xFF19); }

        bool kanji_numeral(char32_t c)
        {
            return c == 0x3007 || std::u32string_view(U"零一二三四五六七八九十百千万億兆").find(c) != std::u32string_view::npos;
        }

        // JLReq Appendix A cl-12 (prefixed abbreviations) and cl-13 (postfixed abbreviations): inseparable from
        // adjacent numerals (JLReq 3.1.10 ④⑤, 3.1.11 ①).
        bool prefix_abbrev(char32_t c)
        {
            switch(c)
            {
            case '#': case '$': case 0x00A3: case 0x00A5: case 0x20AC: case 0x2116: case 0x3012:
            case 0xFF03: case 0xFF04: case 0xFFE1: case 0xFFE5:
                return true;
            default:
                return false;
            }
        }

        bool postfix_abbrev(char32_t c)
        {
            switch(c)
            {
            case '%': case 0x00A2: case 0x00B0: case 0x2030: case 0x2031: case 0x2032: case 0x2033:
            case 0x2103: case 0x2109: case 0xFF05: case 0xFFE0:
                return true;
            default:
                return false;
            }
        }

        bool abbrev_number_pair(char32_t before, char32_t after)
        {
            const auto is_num = [](char32_t c) { return numeric(c) || kanji_numeral(c); };
            return (prefix_abbrev(before) && is_num(after)) || (is_num(before) && postfix_abbrev(after));
        }

        // UAX #29's MidNum and MidNumLet (and the apostrophe): between two digits, part of one number (1.5, 12,000).
        bool mid_num(char32_t c)
        {
            return c == ',' || c == '.' || c == ';' || c == '\'' || c == 0x2018 || c == 0x2019 || c == 0x2024 ||
                   c == 0x2044 || c == 0xFE50 || c == 0xFE52 || c == 0xFE54 || c == 0xFF07 || c == 0xFF0C ||
                   c == 0xFF0E || c == 0xFF1B;
        }

        // Whether the number ending (forward: starting) at byte i is part of a longer word: a letter or digit next to
        // it, or a separator with a digit beyond it.
        bool joins_number(const std::string& text, size_t i, bool forward)
        {
            size_t   n = 0, at = i;
            char32_t c = forward ? (i < text.size() ? decode(text, i, n) : 0) : decode_before(text, i, at);
            if(word_char(c))
                return true;
            if(!mid_num(c))
                return false;
            size_t beyond = 0;
            return numeric(forward ? (i + n < text.size() ? decode(text, i + n, n) : 0) : decode_before(text, at, beyond));
        }

        // A Latin character next to an overlay token (not a space): it is part of a Latin word or a longer run of marks.
        bool latin_next_to(char32_t c)
        {
            return c > ' ' && !is_space(c) && script_of(c) == script::latin;
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
        return (a & not_ending) || (b & not_starting) || (a & b & inseparable) || abbrev_number_pair(before, after);
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
        if((classify(before) & classify(after) & inseparable) || abbrev_number_pair(before, after))
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
        // JLReq 3.1.3: in kanji numerals, a place or approximation 、 and a decimal ・ are set solid.
        if((c == 0x3001 || c == 0x30FB) && kanji_numeral(prev) && kanji_numeral(next))
            return c == 0x3001 ? trim{0, 0.5f} : trim{0.25f, 0.25f};
        // JLReq 3.1.5 ①: a wrapped line's opening bracket is flush with the line head (天ツキ); one that starts a
        // paragraph keeps its first half there (the paragraph's indent before it is line_box's).
        if((is & opening) && (((at & line_start) && !(at & paragraph_start)) || (classify(prev) & punctuation)))
            return {0.5f, 0};
        if((is & first_half) && ((classify(next) & (first_half | middle_dot)) || (at & line_end)))
            return {0, 0.5f};
        return {};
    }

    // stone_engine_js context.ts isTateChuYoko / isTateChuYokoToken: a token of at most 2 runs, each a number
    // (isNumberChar: \p{N}) the Latin font sets (rotated, not notNeedsToClockwiseInTbRl): of those, the ASCII digits
    // (manifest.json "mirrored"). The overlay's tokens are ours.
    std::vector<span> tate_chu_yoko(const std::string& text, size_t from, size_t to)
    {
        std::vector<span> out;
        to = std::min(to, text.size());
        for(size_t i = from, n = 0; i < to; i += n)
        {
            const char32_t c = decode(text, i, n);
            if(ascii_digit(c))
            {
                size_t start = i, end = i, at = 0;
                while(start > 0 && ascii_digit(decode_before(text, start, at)))
                    start = at;
                while(end < text.size() && ascii_digit(static_cast<unsigned char>(text[end])))
                    end++;
                if(end - start <= 2 && start >= from && end <= to && !joins_number(text, start, false) &&
                   !joins_number(text, end, true))
                    out.push_back({start, end});
                n = end - i;
                continue;
            }
            for(const char* token : tate_chu_yoko_tokens)
            {
                if(!token)
                    continue;
                const size_t len = std::char_traits<char>::length(token);
                size_t       at  = 0, m = 0;
                if(i + len <= to && text.compare(i, len, token) == 0 && !latin_next_to(decode_before(text, i, at)) &&
                   !latin_next_to(i + len < text.size() ? decode(text, i + len, m) : 0))
                {
                    out.push_back({i, i + len});
                    n = len;
                    break;
                }
            }
        }
        return out;
    }

    squeeze squeezable(char32_t c, trim taken, uint8_t at)
    {
        const classes is = classify(c);
        if(at & line_end)
            return (is & middle_dot) ? squeeze{0.25f - taken.start, 0.25f - taken.end, squeeze_step::line_end_dot} : squeeze{};
        if(is & middle_dot)
            return {0.25f - taken.start, 0.25f - taken.end, squeeze_step::middle_dot};
        if((is & opening) && !(at & line_start) && taken.start < 0.5f)
            return {0.5f - taken.start, 0, squeeze_step::bracket};
        const bool full_stop = c == 0x3002 || c == 0xFF61;  // 。｡ (cl-06): its half em separates sentences
        if((is & (closing | full_stop_comma)) && !full_stop && taken.end < 0.5f)
            return {0, 0.5f - taken.end, squeeze_step::bracket};
        return {};
    }

    namespace
    {
        // JLReq 3.1.11 ②: cl-01, cl-02, cl-03 (゠ 〜), cl-04 (！？ and the like), cl-05, cl-06, cl-07 and U+3000; the
        // Japanese ones only (script_of), so that Latin text (its brackets, ! ? ‐ –) justifies as it did.
        bool avoids_widening(char32_t c)
        {
            if(script_of(c) != script::japanese)
                return false;
            if(classify(c) & (opening | closing | middle_dot | full_stop_comma))
                return true;
            switch(c)
            {
            case 0x30A0: case 0x301C: case 0xFF5E:                                           // cl-03
            case 0xFF01: case 0xFF1F: case 0x203C: case 0x2047: case 0x2048: case 0x2049:    // cl-04
            case 0x3000:
                return true;
            default:
                return false;
            }
        }
    } // namespace

    gap_kind justify_gap_kind(char32_t before, char32_t after)
    {
        if(avoids_widening(before) || avoids_widening(after))
            return gap_kind::avoided;
        if(before == 0 || after == 0)
            return gap_kind::japanese;
        if(is_space(before) || is_space(after))
            return gap_kind::word_space;
        if(script_of(before) == script::latin || script_of(after) == script::latin)
            return gap_kind::latin;
        return gap_kind::japanese;
    }

    namespace
    {
        // Kana and kanji (cl-15, cl-16, cl-19; ー, 々, 〆): not punctuation, not full-width letters.
        bool kana_or_kanji(char32_t c)
        {
            return (c >= 0x3041 && c <= 0x3096) || (c >= 0x309D && c <= 0x309F) || (c >= 0x30A1 && c <= 0x30FA) ||
                   (c >= 0x30FC && c <= 0x30FF) || c == 0x3005 || c == 0x3006 || c == 0x3007 || (c >= 0x31F0 && c <= 0x31FF) ||
                   (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
                   (c >= 0x20000 && c <= 0x3134F);
        }

        // A Latin letter or digit (cl-24, cl-27): ASCII, Latin-1 and Extended, Greek, Cyrillic.
        bool latin_alnum(char32_t c)
        {
            return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') ||
                   (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) || (c >= 0x370 && c <= 0x3FF) || (c >= 0x400 && c <= 0x4FF);
        }
    } // namespace

    float dividing_space_em(char32_t c, char32_t next, uint8_t at)
    {
        if((at & line_end) || next == 0 || is_space(next) ||
           !(c == 0xFF01 || c == 0xFF1F || c == 0x203C || (c >= 0x2047 && c <= 0x2049)))
            return 0.0f;
        const classes b = classify(next);
        // JLReq 3.1.6 ① / Appendix B: no space before closing brackets, another dividing mark, stops, commas,
        // middle dots, hyphens, small kana (っ in 何？って), etc.
        if(b & not_starting)
            return 0.0f;
        // Before a full-width opening bracket (cl-01), add 0.5em so its built-in leading 0.5em gives 1.0em total.
        if(b & opening)
            return 0.5f;
        return 1.0f;
    }

    bool dividing_space_after(char32_t c, char32_t next, uint8_t at)
    {
        return dividing_space_em(c, next, at) > 0.0f;
    }

    float hyphen_target_em(char32_t c)
    {
        if(c == 0x2010)
            return 0.25f;
        if(c == 0x30A0)
            return 0.5f;
        return 0.0f;
    }

    bool latin_space_after(char32_t c, char32_t next, uint8_t at)
    {
        if(at & line_end)
            return false;
        return (kana_or_kanji(c) && latin_alnum(next)) || (latin_alnum(c) && kana_or_kanji(next));
    }

    bool ruby_may_overhang(char32_t c)
    {
        if(c >= 0x3041 && c <= 0x3096)  // hiragana, small ones too
            return true;
        if(c >= 0x30A1 && c <= 0x30FA)  // katakana, small ones too
            return true;
        if(c == 0x30FC || (c >= 0x31F0 && c <= 0x31FF))  // ー, small katakana for Ainu
            return true;
        return c >= 0xFF66 && c <= 0xFF9D;  // half-width katakana and ｰ
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
