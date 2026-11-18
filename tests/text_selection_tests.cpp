// litehtml without TwidgetCore: text selection (litehtml::text_selection) over
// horizontal and vertical-rl documents with ruby, against a stub container that measures 8 px a byte.
#include <litehtml.h>
#include <text_selection.h>

#include <cstdio>
#include <string>

using namespace litehtml;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) failures++;
}

struct Stub : document_container {
    float w = 300, h = 200;
    uint_ptr create_font(const font_description& d, const document*, font_metrics* fm) override {
        if (fm) {
            fm->font_size = d.size;
            fm->ascent = d.size * 0.8f;
            fm->descent = d.size * 0.2f;
            fm->height = d.size;
            fm->x_height = d.size * 0.5f;
            fm->ch_width = 8;
        }
        return 1;
    }
    void delete_font(uint_ptr) override {}
    pixel_t text_width(const char* text, uint_ptr) override { return pixel_t(8.0f * float(std::string(text).size())); }
    void draw_text(uint_ptr, const char*, uint_ptr, web_color, const position&) override {}
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

// あい漢(かん)う。えお。 : 3 bytes a character, 24 px each at 8 px a byte; the annotation is half size, still 24 px each.
const char* BODY = "<p style=\"margin:0\">\xE3\x81\x82\xE3\x81\x84<ruby>\xE6\xBC\xA2<rt>\xE3\x81\x8B\xE3\x82\x93</rt></ruby>"
                   "\xE3\x81\x86\xE3\x80\x82\xE3\x81\x88\xE3\x81\x8A\xE3\x80\x82</p>";

} // namespace

int main() {
    const std::string sentence2 = "\xE3\x81\x88\xE3\x81\x8A\xE3\x80\x82";  // えお。
    const std::string base = "\xE3\x81\x82\xE3\x81\x84\xE6\xBC\xA2\xE3\x81\x86\xE3\x80\x82";  // あい漢う。
    {
        Stub c;
        auto doc = document::createFromString(std::string("<html><body style=\"margin:0;font-size:16px;line-height:20px\">") + BODY + "</body></html>", &c);
        doc->render(300);
        text_selection ts(*doc);
        // The line: あ 0-24, い 24-48, the pair 漢 (its annotation かん is 48 px, 24 more than 漢: it overhangs い and う by
        // one ruby character, 8 px, each, JLReq 3.3.8: the pair 40-88), う。 80-128 (one word), えお。 128-200.
        text_position a = ts.position_at(1, 30), b = ts.position_at(127, 30);
        check(ts.text(a, b) == base, "horizontal: two points select the base text, the annotation left out (" + ts.text(a, b) + ")");
        auto rects = ts.rects(a, b);
        check(rects.size() == 1 && float(rects[0].x) == 0 && float(rects[0].width) == 128, "horizontal: one rectangle for the line, over the ruby pair");
        text_position s0, s1;
        check(ts.sentence(ts.position_at(170, 30), s0, s1) && ts.text(s0, s1) == sentence2, "horizontal: the sentence at a point");
        check(ts.sentence(ts.position_at(60, 30), s0, s1) && ts.text(s0, s1) == base, "horizontal: the sentence across a ruby");
    }
    {
        Stub c;  // a 300 x 200 page: the column is 200 px long, from the right edge
        auto doc = document::createFromString(std::string("<html style=\"writing-mode:vertical-rl\"><body style=\"margin:0;font-size:16px;line-height:20px\">") + BODY + "</body></html>", &c);
        doc->render(300);
        check(doc->vertical(), "vertical-rl: the document is vertical");
        text_selection ts(*doc);
        // The first column is x 280-300; its characters go down: あ at y 0-24, い 24-48.
        text_position a = ts.position_at(290, 1), b = ts.position_at(290, 127);
        check(ts.text(a, b) == base, "vertical: two points down the column select the base text (" + ts.text(a, b) + ")");
        auto rects = ts.rects(a, b);
        check(rects.size() == 1 && float(rects[0].y) == 0 && float(rects[0].height) == 128 && float(rects[0].x) >= 280 &&
                  float(rects[0].x) + float(rects[0].width) <= 300,
              "vertical: one rectangle down the first column, on the page");
        text_position s0, s1;
        check(ts.sentence(ts.position_at(290, 170), s0, s1) && ts.text(s0, s1) == sentence2, "vertical: the sentence at a point");
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
