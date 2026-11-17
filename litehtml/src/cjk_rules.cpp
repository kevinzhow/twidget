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
} // namespace litehtml::cjk
