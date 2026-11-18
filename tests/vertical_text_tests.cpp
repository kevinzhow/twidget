// litehtml without TwidgetCore: which characters of vertical-rl text are drawn upright (turned back in a group about
// their em box) and which sideways, against a stub container that measures 8 px a character and says whether the font
// drawing a character has a vertical form for it (document_container::has_vertical_form).
#include <litehtml.h>

#include <cmath>
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

const std::string ellipsis = "\xE2\x80\xA6", leader = "\xE2\x80\xA5";

struct Stub : document_container {
    float w = 300, h = 400;
    bool latin_ellipsis = false;  // … and ‥ come from a font without vertical forms (a Latin font first in the list)
    std::vector<transform_matrix> groups;
    std::string drawn;  // each text drawn: "up:" (in a group turning it back upright) or "side:", and the text
    std::string placed;  // each text drawn, "text@x" (x along the column)
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
    // 8 px a (3-byte) character; あっ is kerned to 14 px, as a horizontal shaping kerns a pair (Noto Sans CJK's まっ).
    pixel_t text_width(const char* text, uint_ptr) override {
        if (std::string(text) == "\xE3\x81\x82\xE3\x81\xA3") return 14;
        return pixel_t(8.0f * float(std::string(text).size() / 3));
    }
    bool has_vertical_form(const char* text, uint_ptr) override {
        return !latin_ellipsis || (text != ellipsis && text != leader);
    }
    void begin_group(uint_ptr, const paint_group& group) override { groups.push_back(group.matrix); }
    void end_group(uint_ptr) override { groups.pop_back(); }
    void draw_text(uint_ptr, const char* text, uint_ptr, web_color, const position& pos) override {
        const bool up = !groups.empty() && groups.back().a == 0 && groups.back().b == -1;  // upright_matrix
        drawn += (drawn.empty() ? "" : " ") + std::string(up ? "up:" : "side:") + text;
        placed += (placed.empty() ? "" : " ") + std::string(text) + "@" + std::to_string(int(std::lround(float(pos.x))));
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
    void get_language(std::string& lang, std::string& culture) const override { lang = "ja"; culture.clear(); }
};

std::string draw(const std::string& text, bool latin_ellipsis, bool placed = false) {
    Stub c;
    c.latin_ellipsis = latin_ellipsis;
    auto doc = document::createFromString("<html style=\"writing-mode:vertical-rl\"><body style=\"margin:0;font-size:16px\"><p style=\"margin:0\">" +
                                              text + "</p></body></html>",
                                          &c);
    doc->render(300);
    doc->draw(0, 0, 0, nullptr);
    return placed ? c.placed : c.drawn;
}

} // namespace

int main() {
    // … ‥ (UAX #50 Tr): upright with the font's vertical form; sideways when the font has none, as Blink draws a Latin
    // font's … (an upright one is dots across the column). ああ stays upright either way.
    const std::string a = "\xE3\x81\x82";
    std::string got = draw(a + ellipsis + ellipsis + a + leader + a, false);
    check(got == "up:" + a + " up:" + ellipsis + " up:" + ellipsis + " up:" + a + " up:" + leader + " up:" + a,
          "with vertical forms, upright: " + got);
    got = draw(a + ellipsis + ellipsis + a + leader + a, true);
    check(got == "up:" + a + " side:" + ellipsis + ellipsis + " up:" + a + " side:" + leader + " up:" + a,
          "without, sideways (…… one run): " + got);

    // Upright characters are not kerned (Blink shapes vertical text top to bottom: no pair adjustments), within a piece
    // either: あっ (one piece, っ does not start a line) takes 16 px, and い follows at 16, not on っ at 14.
    const std::string tsu = "\xE3\x81\xA3", i = "\xE3\x81\x84";
    got = draw(a + tsu + i, false, true);
    check(got == a + "@0 " + tsu + "@8 " + i + "@16", "upright characters are not kerned: " + got);

    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
