#ifndef LITEHTML_STYLESHEET_H
#define LITEHTML_STYLESHEET_H

#include <unordered_map>
#include <vector>

#include <utility>

#include "css_selector.h"
#include "css_tokenizer.h"

namespace litehtml
{

    // https://www.w3.org/TR/cssom-1/#css-declarations
    struct raw_declaration
    {
        using vector = std::vector<raw_declaration>;

        // property name
        std::string name;
        // default value is specified here to get rid of gcc warning "missing initializer for member"
        css_token_vector value;

        bool important = false;

        operator bool() const
        {
            return name != "";
        }
    };

    // intermediate half-parsed rule that is used internally by the parser
    class raw_rule
    {
      public:
        using ptr    = std::shared_ptr<raw_rule>;
        using vector = std::vector<ptr>;

        enum rule_type
        {
            qualified,
            at
        };

        raw_rule(rule_type type, std::string name = {}) :
            type(type),
            name(std::move(name))
        {
        }

        rule_type type;
        // An at-rule has a name, a prelude consisting of a list of component values, and an optional block consisting
        // of a simple {} block.
        std::string name;
        // https://www.w3.org/TR/css-syntax-3/#qualified-rule
        // A qualified rule has a prelude consisting of a list of component values, and a block consisting of a simple
        // {} block. Note: Most qualified rules will be style rules, where the prelude is a selector and the block a
        // list of declarations.
        css_token_vector prelude;
        css_token        block;
    };

    class css
    {
        css_selector::vector m_selectors;

      public:
        /// The sorted selectors by what their rightmost compound needs first, as html_tag::apply_selector rejects
        /// them: a class (by_class), else a tag (by_tag), else nothing cheap to check (other). Positions in
        /// selectors(), ascending. Built by sort_selectors().
        struct selector_index
        {
            std::unordered_map<string_id, std::vector<uint32_t>> by_class, by_tag;
            std::vector<uint32_t>                                other;
            bool                                                 built = false;
        };

        const css_selector::vector& selectors() const
        {
            return m_selectors;
        }
        const selector_index& index() const { return m_index; }

        template <class Input>
        void parse_css_stylesheet(const Input& input, const std::string& baseurl, const std::shared_ptr<document>& doc,
                                  const media_query_list_list::ptr& media = nullptr, bool top_level = true);

        void sort_selectors();

      private:
        bool parse_style_rule(const raw_rule::ptr& rule, const std::string& baseurl,
                              const std::shared_ptr<document>& doc, const media_query_list_list::ptr& media);
        void parse_import_rule(const raw_rule::ptr& rule, const std::string& baseurl,
                               const std::shared_ptr<document>& doc, const media_query_list_list::ptr& media);
        void add_selector(const css_selector::ptr& selector);

        int m_layer_depth = 0;  // inside an @layer block
        selector_index m_index;
    };

    inline void css::add_selector(const css_selector::ptr& selector)
    {
        m_index.built = false;
        selector->m_order = static_cast<int>(m_selectors.size());
        selector->m_layered = m_layer_depth > 0;
        m_selectors.push_back(selector);
    }

} // namespace litehtml

#endif // LITEHTML_STYLESHEET_H
