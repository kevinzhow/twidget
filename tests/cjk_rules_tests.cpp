// litehtml without TwidgetCore: where a line may break between two elements' texts in Japanese, by the rule set
// (litehtml/cjk_rules.h: stone_engine's kinsoku tables plus our overlay). The stub container measures 8 px a character
// and cuts its text after each character from U+2000, so every break between two characters here is litehtml's choice
// at an element boundary.
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

// The pieces drawn for a paragraph, each as "text@x,y" (lines at y 2, 22, ...: 16 px text in 20 px lines).
std::string layout(const std::string& width, const std::string& html) {
    Stub c;
    auto doc = document::createFromString("<html><body style=\"margin:0;font-size:16px;line-height:20px\"><p style=\"margin:0;width:" +
                                              width + "\">" + html + "</p></body></html>",
                                          &c);
    doc->render(300);
    doc->draw(0, 0, 0, nullptr);
    std::string out;
    for (const auto& d : c.draws) out += (out.empty() ? "" : " ") + d;
    return out;
}

} // namespace

int main() {
    // 漢漢 then a and b in a line 3 characters wide: b does not fit after a. Where no line may break between a and b,
    // a goes to the next line with b; elsewhere b starts the next line.
    const std::string kan = "\xE6\xBC\xA2";
    const std::string together_tail = kan + "@0,2 " + kan + "@8,2 ";
    struct Pair {
        const char* a;
        const char* b;
        bool together;
        const char* why;
    };
    const Pair pairs[] = {
        {"サ", "ー", true, "ー does not start a line (サ|ービス)"},
        {"困", "っ", true, "small っ does not start a line (困|った)"},
        {"テ", "ィ", true, "small ィ does not start a line (プロパテ|ィ)"},
        {"じ", "ゃ", true, "small ゃ does not start a line (じ|ゃ)"},
        {"あ", "。", true, "。 does not start a line"},
        {"あ", "」", true, "」 does not start a line"},
        {"「", "あ", true, "「 does not end a line"},
        {"…", "…", true, "…… is not split"},
        {"―", "―", true, "―― (U+2015) is not split"},
        {"─", "─", true, "── (U+2500, the overlay) is not split"},
        {"9", "%", true, "% does not start a line after a number"},
        {"a", ".", true, ". does not start a line"},
        {"漢", "字", false, "a line may break between two kanji"},
        {"あ", "い", false, "a line may break between two kana"},
        {"。", "あ", false, "a line may break after 。"},
    };
    for (const auto& p : pairs) {
        const std::string a = p.a, b = p.b;
        const std::string want = p.together ? together_tail + a + "@0,22 " + b + "@8,22" : together_tail + a + "@16,2 " + b + "@0,22";
        const std::string got = layout("24px", kan + kan + "<span>" + a + "</span>" + b);
        check(got == want, std::string(p.why) + " across </span>: " + got);
    }
    // The boundary is an element's start, or several.
    std::string got = layout("24px", kan + kan + "サ<span>ー</span>");
    check(got == together_tail + "サ@0,22 ー@8,22", "ー does not start a line across <span>: " + got);
    got = layout("24px", kan + kan + "<b><i>「</i></b><span>あ</span>");
    check(got == together_tail + "「@0,22 あ@8,22", "「 does not end a line across </i></b><span>: " + got);
    // A space after the element is no character the next line would start with: the line breaks there as before.
    got = layout("16px", "一<span>二</span> 三");
    check(got == "一@0,2 二@8,2 三@0,22", "a space after an element: " + got);

    // The classes the steps after line breaking (justify, punctuation compression) read.
    using namespace litehtml::cjk;
    const std::pair<char32_t, classes> classes_of[] = {
        {U'「', not_ending | opening},
        {U'」', not_starting | closing},
        {U'（', not_ending | opening},
        {U'）', not_starting | closing},
        {U'。', not_starting | full_stop_comma | hanging},
        {U'、', not_starting | full_stop_comma | hanging},
        {U'・', not_starting | middle_dot},
        {U'：', not_starting | middle_dot},
        {U'ー', not_starting},
        {U'…', inseparable},
        {U'─', inseparable},
        {U'漢', 0},
        {U'a', 0},
    };
    for (auto [c, want] : classes_of) {
        char what[64];
        std::snprintf(what, sizeof what, "classify(U+%04X) == %#x (got %#x)", unsigned(c), unsigned(want), unsigned(classify(c)));
        check(classify(c) == want, what);
    }

    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
