// litehtml without TwidgetCore: the pieces of a text node (a word, a CJK character, a soft-hyphenated syllable) cut from
// one shaped run, and where they are drawn. The stub container measures 8 px a character (a soft hyphen 0) and shapes a
// run as HarfBuzz does with a book font: ―― is one ligature glyph, and so is f<soft hyphen>f (Noto Serif's ff), each a
// cluster with no advance between its characters (document_container::text_advances).
#include <litehtml.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace litehtml;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) failures++;
}

const char32_t SHY = 0xAD, BAR = 0x2015;

std::vector<std::pair<char32_t, size_t>> code_points(const std::string& s) {  // each with its first byte
    std::vector<std::pair<char32_t, size_t>> out;
    for (size_t at = 0; at < s.size();) {
        const unsigned char c = (unsigned char)s[at];
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        char32_t cp = n == 1 ? c : n == 2 ? c & 0x1f : n == 3 ? c & 0x0f : c & 0x07;
        for (size_t k = 1; k < n; k++) cp = (cp << 6) | ((unsigned char)s[at + k] & 0x3f);
        out.push_back({cp, at});
        at += n;
    }
    return out;
}

struct Draw {
    std::string text;
    float x, y;
};

struct Stub : document_container {
    float w = 300, h = 400;
    std::vector<Draw> draws;
    uint_ptr create_font(const font_description& d, const document*, font_metrics* fm) override {
        if (fm) {
            fm->font_size = d.size;
            fm->ascent = d.size * 0.8f;
            fm->descent = d.size * 0.2f;
            fm->height = d.size;
            fm->x_height = d.size * 0.5f;
            fm->ch_width = 8;
            fm->draw_spaces = false;
        }
        return 1;
    }
    void delete_font(uint_ptr) override {}
    // Alone: 8 px a character, a soft hyphen invisible.
    pixel_t text_width(const char* text, uint_ptr) override {
        float width = 0;
        for (auto [cp, at] : code_points(text)) width += cp == SHY ? 0 : 8;
        return width;
    }
    // In a run: ―― is a 16 px ligature and f<soft hyphen>f a 12 px one; inside a cluster there is no advance (NaN).
    bool text_advances(const std::string& text, uint_ptr, std::vector<pixel_t>& prefix) override {
        const auto cps = code_points(text);
        prefix.assign(text.size() + 1, std::numeric_limits<float>::quiet_NaN());
        float sum = 0;
        for (size_t i = 0; i < cps.size();) {
            size_t n = 1;
            float advance = cps[i].first == SHY ? 0 : 8;
            if (i + 1 < cps.size() && cps[i].first == BAR && cps[i + 1].first == BAR) n = 2, advance = 16;
            if (i + 2 < cps.size() && cps[i].first == 'f' && cps[i + 1].first == SHY && cps[i + 2].first == 'f') n = 3, advance = 12;
            prefix[cps[i].second] = sum;
            sum += advance;
            i += n;
        }
        prefix[text.size()] = sum;
        return true;
    }
    // A break opportunity at a space, after a soft hyphen and after each character from U+2000 (CJK, dashes).
    void split_text(const char* text, const std::function<void(const char*)>& on_word,
                    const std::function<void(const char*)>& on_space) override {
        const std::string s = text;
        size_t start = 0;
        for (auto [cp, at] : code_points(s)) {
            const size_t end = at + (cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4);
            if (cp == ' ') {
                if (at > start) on_word(s.substr(start, at - start).c_str());
                on_space(" ");
                start = end;
            } else if (cp == SHY || cp >= 0x2000) {
                on_word(s.substr(start, end - start).c_str());
                start = end;
            }
        }
        if (s.size() > start) on_word(s.substr(start).c_str());
    }
    void draw_text(uint_ptr, const char* text, uint_ptr, web_color, const position& pos) override {
        draws.push_back({text, float(pos.x), float(pos.y)});
    }
    pixel_t pt_to_px(float pt) const override { return pt * 96 / 72; }
    pixel_t get_default_font_size() const override { return 16; }
    const char* get_default_font_name() const override { return "serif"; }
    void draw_list_marker(uint_ptr, const list_marker&) override {}
    void load_image(const char*, const char*, bool) override {}
    void get_image_size(const char*, const char*, size& sz) override { sz = {}; }
    void draw_image(uint_ptr, const background_layer&, const std::string&, const std::string&) override {}
    void draw_solid_fill(uint_ptr, const background_layer&, const web_color&) override {}
    void draw_linear_gradient(uint_ptr, const background_layer&, const background_layer::linear_gradient&) override {}
    void draw_radial_gradient(uint_ptr, const background_layer&, const background_layer::radial_gradient&) override {}
    void draw_conic_gradient(uint_ptr, const background_layer&, const background_layer::conic_gradient&) override {}
    void draw_borders(uint_ptr, const borders&, const position&, bool) override {}
    void set_caption(const char*) override {}
    void set_base_url(const char*) override {}
    void link(const std::shared_ptr<document>&, const element::ptr&) override {}
    void on_anchor_click(const char*, const element::ptr&) override {}
    void on_mouse_event(const element::ptr&, mouse_event) override {}
    void set_cursor(const char*) override {}
    void transform_text(std::string&, text_transform) override {}
    void import_css(std::string&, const std::string&, std::string&) override {}
    void set_clip(const position&, const border_radiuses&) override {}
    void del_clip() override {}
    void get_viewport(position& v) const override { v = position(0, 0, w, h); }
    element::ptr create_element(const char*, const string_map&, const std::shared_ptr<document>&) override { return nullptr; }
    void get_media_features(media_features& m) const override {
        m.type = media_type_screen;
        m.width = m.device_width = w;
        m.height = m.device_height = h;
    }
    void get_language(std::string& lang, std::string& culture) const override { lang = "en"; culture.clear(); }
};

// The pieces drawn for a paragraph, each as "text@x,y" (a line's text is at y 2 and 22 ...: 16 px in 20 px lines).
std::string layout(const std::string& style, const std::string& text) {
    Stub c;
    auto doc = document::createFromString("<html><body style=\"margin:0;font-size:16px;line-height:20px\"><p style=\"margin:0;" + style +
                                              "\">" + text + "</p></body></html>",
                                          &c);
    doc->render(300);
    doc->draw(0, 0, 0, nullptr);
    std::string out;
    char buf[64];
    for (const auto& d : c.draws) {
        std::snprintf(buf, sizeof buf, "@%g,%g", d.x, d.y);
        out += (out.empty() ? "" : " ") + d.text + buf;
    }
    return out;
}

const std::string shy = "\xC2\xAD", bar = "\xE2\x80\x95";

} // namespace

int main() {
    // A ligature across a break opportunity: each piece is drawn alone, so it takes the width it is drawn with (―― in a
    // CJK font: the second ― was drawn under 步; ff across a soft hyphen: the next word was drawn over "fr").
    std::string got = layout("", "\xE3\x81\xAF" + bar + bar + "\xE6\xAD\xA5");
    check(got == "\xE3\x81\xAF@0,2 " + bar + "@8,2 " + bar + "@16,2 \xE6\xAD\xA5@24,2", "a ligature cut by a break: " + got);
    got = layout("", "af" + shy + "fr gh");
    check(got == "af" + shy + "@0,2 fr@16,2 gh@40,2", "a ligature across a soft hyphen: " + got);

    // A soft hyphen is invisible unless the line breaks there; then a hyphen is drawn at the line's end, and the break is
    // taken only when the hyphen fits (as Blink).
    got = layout("width:48px", "ab cd" + shy + "ef");
    check(got == "ab@0,2 cd-@24,2 ef@0,22", "a line broken at a soft hyphen ends with a hyphen: " + got);
    got = layout("width:44px", "ab cd" + shy + "ef");
    check(got == "ab@0,2 cd" + shy + "@0,22 ef@16,22", "a soft hyphen whose hyphen does not fit is no break: " + got);
    got = layout("width:50px;text-align:justify", "ab cd" + shy + "ef");
    check(got == "ab@0,2 cd-@26,2 ef@0,22", "justified, the hyphen ends at the line's end: " + got);
    // Justified, a soft hyphen inside a line takes no space: the word stays whole, the spaces take it all.
    got = layout("width:64px;text-align:justify", "ab" + shy + "cd ef gh");
    check(got == "ab" + shy + "@0,2 cd@16,2 ef@48,2 gh@0,22", "justified, no space at a soft hyphen: " + got);

    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
