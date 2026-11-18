// litehtml without TwidgetCore: no line breaks where there is no break opportunity between two pieces of text, also
// when the line is too narrow for them (CSS Text 3 §5: then they overflow), so that a table column's minimum width
// (laid out at no width) is the widest unbreakable run. No break opportunity: inside white-space: nowrap or pre (the
// nearest common ancestor of the two characters says, §5.1), between two letters under word-break: keep-all, or
// where the CJK rule set forbids one (kinsoku). Kevin's 活用 table (Pengdu, 三类动词): a column of
// <span style="white-space:nowrap">する</span> bubbles and a 词尾 header with word-break: keep-all, next to a column
// of width: 100%, was one character wide, する and 词尾 broken in two. The stub container measures 8 px a character
// and cuts its text after each character from U+2000, as twidget's does for CJK text.
#include <litehtml.h>
#include <litehtml/cjk_rules.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace litehtml;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) failures++;
}

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

struct Stub : document_container {
    std::vector<std::string> draws;
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
    pixel_t text_width(const char* text, uint_ptr) override { return pixel_t(8.0f * float(code_points(text).size())); }
    // A break opportunity at a space and after each character from U+2000 (CJK, dashes).
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
            } else if (cp >= 0x2000) {
                on_word(s.substr(start, end - start).c_str());
                start = end;
            }
        }
        if (s.size() > start) on_word(s.substr(start).c_str());
    }
    void draw_text(uint_ptr, const char* text, uint_ptr, web_color, const position& pos) override {
        char at[32];
        std::snprintf(at, sizeof at, "@%g,%g", float(pos.x), float(pos.y));
        draws.push_back(text + std::string(at));
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
    void get_viewport(position& v) const override { v = position(0, 0, 300, 400); }
    element::ptr create_element(const char*, const string_map&, const std::shared_ptr<document>&) override { return nullptr; }
    void get_media_features(media_features& m) const override {
        m.type = media_type_screen;
        m.width = m.device_width = 300;
        m.height = m.device_height = 400;
    }
    void get_language(std::string& lang, std::string& culture) const override { lang = "ja"; culture.clear(); }
};

// The pieces drawn for a body, each as "text@x,y" (lines at y 2, 22, ...: 16 px text in 20 px lines).
std::string body(const std::string& html) {
    Stub c;
    auto doc = document::createFromString("<html><body style=\"margin:0;font-size:16px;line-height:20px\">" + html + "</body></html>", &c);
    doc->render(300);
    doc->draw(0, 0, 0, nullptr);
    std::string out;
    for (const auto& d : c.draws) out += (out.empty() ? "" : " ") + d;
    return out;
}

// Where a piece with exactly this text was drawn ("x,y"), or "" when none was.
std::string drawn_at(const std::string& draws, const std::string& text) {
    for (size_t at = 0; at <= draws.size();) {
        size_t end = draws.find(' ', at);
        if (end == std::string::npos) end = draws.size();
        const std::string piece = draws.substr(at, end - at);
        const size_t sign = piece.rfind('@');
        if (sign != std::string::npos && piece.substr(0, sign) == text) return piece.substr(sign + 1);
        at = end + 1;
    }
    return "";
}

// A paragraph `width` wide.
std::string layout(const std::string& width, const std::string& html) {
    return body("<p style=\"margin:0;width:" + width + "\">" + html + "</p>");
}

// A one-row table 300 px wide, no spacing or padding: `first` in the first cell, 例 in the second (width: 100%, so the
// first column is as narrow as it may be: its minimum width). 例's x is the first column's width (the cell is
// vertically centred, so its y follows the row's height).
std::string table(const std::string& first) {
    return body("<table style=\"width:300px;border-spacing:0\"><tr><td style=\"padding:0\">" + first +
                "</td><td style=\"padding:0;width:100%\">\xE4\xBE\x8B</td></tr></table>");
}

} // namespace

int main() {
    const std::string su = "\xE3\x81\x99", ru = "\xE3\x82\x8B", rei = "\xE4\xBE\x8B";
    const std::string ci = "\xE8\xAF\x8D", wei = "\xE5\xB0\xBE";  // 词尾
    const std::string a = "\xE3\x81\x82", i = "\xE3\x81\x84", u = "\xE3\x81\x86", e = "\xE3\x81\x88", o = "\xE3\x81\x8A";
    const std::string maru = "\xE3\x80\x82", ten = "\xE3\x80\x81";
    const std::string nowrap = "<span style=\"white-space:nowrap\">";

    // Kevin's table: the first column as wide as its widest unbreakable run, which stays on one line.
    std::string got = table(nowrap + su + ru + "</span>");
    check(drawn_at(got, su) == "0,2" && drawn_at(got, ru) == "8,2" && drawn_at(got, rei).rfind("16,", 0) == 0,
          "a nowrap する: a column two characters wide, on one line: " + got);
    got = table("<b style=\"word-break:keep-all\">" + ci + wei + "</b>");
    check(drawn_at(got, ci) == "0,2" && drawn_at(got, wei) == "8,2" && drawn_at(got, rei).rfind("16,", 0) == 0,
          "词尾 with word-break: keep-all: a column two characters wide, on one line: " + got);
    // Across an element's boundary (inside one text the container's split_text decides by the language).
    got = table(a + "<span>" + maru + "</span>");
    check(drawn_at(got, a) == "0,2" && drawn_at(got, maru) == "8,2" && drawn_at(got, rei).rfind("16,", 0) == 0,
          "あ<span>。</span> (。 does not start a line): a column two characters wide: " + got);
    // Breaks that are allowed still set the minimum: one character, where any two may break.
    got = table(a + i);
    check(drawn_at(got, a) == "0,2" && drawn_at(got, i) == "0,22" && drawn_at(got, rei).rfind("8,", 0) == 0,
          "あい: a column one character wide: " + got);
    got = table(nowrap + su + ru + "</span>" + a);
    check(drawn_at(got, su) == "0,2" && drawn_at(got, ru) == "8,2" && drawn_at(got, a) == "0,22",
          "a line may break after a nowrap span (their common ancestor wraps): " + got);

    // In a paragraph: a nowrap span that does not fit goes to the next line whole.
    got = layout("40px", a + i + u + nowrap + e + o + a + "</span>");
    check(got == a + "@0,2 " + i + "@8,2 " + u + "@16,2 " + e + "@0,22 " + o + "@8,22 " + a + "@16,22",
          "a nowrap えおあ that does not fit after あいう starts the next line: " + got);
    // Wider than the line: it overflows, on one line.
    got = layout("16px", nowrap + a + i + u + "</span>");
    check(got == a + "@0,2 " + i + "@8,2 " + u + "@16,2", "a nowrap あいう wider than the line overflows it: " + got);
    // keep-all: no break between letters; after 、 a line may break.
    got = layout("32px", "<span style=\"word-break:keep-all\">" + a + i + ten + u + e + o + "</span>");
    check(got == a + "@0,2 " + i + "@8,2 " + ten + "@16,2 " + u + "@0,22 " + e + "@8,22 " + o + "@16,22",
          "keep-all: あい、 then うえお on the next line: " + got);
    // Without them, as before: a break between any two kana.
    got = layout("40px", a + i + u + "<span>" + e + o + a + "</span>");
    check(got == a + "@0,2 " + i + "@8,2 " + u + "@16,2 " + e + "@24,2 " + o + "@32,2 " + a + "@0,22",
          "no nowrap: あいうえお then あ: " + got);

    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
