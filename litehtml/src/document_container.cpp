#include "utf8_strings.h"
#include "document_container.h"

namespace
{
    // Characters a line may break around on their own: the CJK scripts (ideographs, kana, Hangul syllables, Yi,
    // bopomofo, CJK symbols and punctuation, fullwidth forms).
    bool cjk_breakable(char32_t c)
    {
        return (c >= 0x2E80 && c <= 0x2FFF) || (c >= 0x3000 && c <= 0x303F && c != 0x3000) || (c >= 0x3040 && c <= 0x31FF) ||
               (c >= 0x3200 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
               (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
               (c >= 0xFF01 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x20000 && c <= 0x3FFFD);
    }

    // No break before (UAX #14 CL, CP, EX, IS, NS and CJ): closing brackets, the full stop and comma, iteration marks,
    // the prolonged sound mark, small kana, fullwidth closing punctuation; and the ideographic space (BA), which ends
    // the line it follows (。　 at a paragraph's turn).
    bool cjk_no_break_before(char32_t c)
    {
        static const char32_t list[] = {0x3000, 0x3001, 0x3002, 0x3005, 0x3009, 0x300B, 0x300D, 0x300F, 0x3011, 0x3015, 0x3017, 0x3019,
                                        0x301B, 0x301E, 0x301F, 0x303B, 0x303C, 0x309B, 0x309C, 0x309D, 0x309E, 0x30A0, 0x30FB,
                                        0x30FC, 0x30FD, 0x30FE, 0xFF01, 0xFF09, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF1F, 0xFF3D,
                                        0xFF5D, 0xFF60, 0xFF61, 0xFF63, 0xFF64, 0x2019, 0x201D, 0x00BB, 0x0021, 0x003F, 0x0029,
                                        0x005D, 0x007D, 0x002C, 0x002E, 0x003A, 0x003B};
        for(char32_t x : list)
            if(c == x)
                return true;
        // Small kana
        static const char32_t small[] = {0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085, 0x3087, 0x308E, 0x3095,
                                         0x3096, 0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, 0x30C3, 0x30E3, 0x30E5, 0x30E7, 0x30EE,
                                         0x30F5, 0x30F6};
        for(char32_t x : small)
            if(c == x)
                return true;
        return c >= 0x31F0 && c <= 0x31FF;
    }

    // No break after (UAX #14 OP): opening brackets.
    bool cjk_no_break_after(char32_t c)
    {
        static const char32_t list[] = {0x3008, 0x300A, 0x300C, 0x300E, 0x3010, 0x3014, 0x3016, 0x3018, 0x301A, 0x301D,
                                        0xFF08, 0xFF3B, 0xFF5B, 0xFF5F, 0xFF62, 0x2018, 0x201C, 0x00AB};
        for(char32_t x : list)
            if(c == x)
                return true;
        return false;
    }
} // namespace

void litehtml::document_container::split_text(const char* text, const std::function<void(const char*)>& on_word,
                                              const std::function<void(const char*)>& on_space)
{
    std::u32string str;
    std::u32string str_in = static_cast<const char32_t*>(utf8_to_utf32(text));
    auto           letter = [](char32_t ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= 0xC0 && ch != 0xD7 && ch != 0xF7 && ch < 0x2000);
    };
    for(size_t i = 0; i < str_in.size(); i++)
    {
        const char32_t c = str_in[i];
        const char32_t prev = i > 0 ? str_in[i - 1] : 0;
        const char32_t next = i + 1 < str_in.size() ? str_in[i + 1] : 0;
        if(((c == '-' || c == 0x2010) && letter(prev) && letter(next)) || (c == '?' && next > ' ' && prev > ' '))
        {
            str += c;
            on_word(utf32_to_utf8(str));
            str.clear();
            continue;
        }
        if(c <= ' ' && (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'))
        {
            if(!str.empty())
            {
                on_word(utf32_to_utf8(str));
                str.clear();
            }
            str += c;
            on_space(utf32_to_utf8(str));
            str.clear();
        }
        // CJK: a line may break before and after each character (UAX #14 classes ID, H2, H3, and CJ as in
        // line-break: normal), except before closing punctuation and small kana, and after opening punctuation.
        // OBSERVED: only U+4E00..9FCC broke, so a run of kana was one word (日本語 alone on a line, then the rest).
        else if(cjk_breakable(c))
        {
            if(cjk_no_break_before(c) && !str.empty())
            {
                str += c;
                continue;
            }
            if(!str.empty() && !cjk_no_break_after(str.back()))
            {
                on_word(utf32_to_utf8(str));
                str.clear();
            }
            str += c;
            // A run of other letters after an opening bracket stays whole too: （abc） is one word.
            auto other = [&](char32_t x) { return x > ' ' && !cjk_breakable(x); };
            while(i + 1 < str_in.size() && str_in[i + 1] > ' ' &&
                  (cjk_no_break_before(str_in[i + 1]) || cjk_no_break_after(str.back()) ||
                   (str.size() > 1 && other(str.back()) && other(str_in[i + 1]))))
                str += str_in[++i];
            if(!cjk_no_break_after(str.back()))
            {
                on_word(utf32_to_utf8(str));
                str.clear();
            }
        } else
        {
            str += c;
        }
    }
    if(!str.empty())
    {
        on_word(utf32_to_utf8(str));
    }
}
