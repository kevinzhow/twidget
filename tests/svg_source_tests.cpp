// litehtml's <svg> (el_svg) parsed from source text, without TwidgetCore: what a container is asked to draw for it, against
// a stub container that records the paths it gets and may draw an <svg> itself (document_container::draw_svg).
#include <litehtml.h>

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

struct Stub : document_container {
    std::vector<vector_path> paths;
    bool draws_svg = false;                   // draw_svg answers true: the container drew it
    std::vector<std::string> svgs;            // the tag of each element draw_svg was given
    std::vector<position> svg_boxes;
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
    void get_viewport(position& v) const override { v = position(0, 0, 300, 200); }
    element::ptr create_element(const char*, const string_map&, const std::shared_ptr<document>&) override { return nullptr; }
    void get_media_features(media_features& m) const override {
        m.type = media_type_screen;
        m.width = m.device_width = 300;
        m.height = m.device_height = 200;
    }
    void get_language(std::string& lang, std::string& culture) const override { lang = "en"; culture.clear(); }
    void draw_path(uint_ptr, const vector_path& path) override { paths.push_back(path); }
    bool draw_svg(uint_ptr, const element::ptr& svg, const position& pos) override {
        svgs.push_back(svg->get_tagName());
        svg_boxes.push_back(pos);
        return draws_svg;
    }
};

/// `body` laid out 300 px wide and drawn into `c`.
void draw(Stub& c, const std::string& body) {
    auto doc = document::createFromString("<html><body style=\"margin:0\">" + body + "</body></html>", &c);
    doc->render(300);
    position clip(0, 0, 300, 200);
    doc->draw(0, 0, 0, &clip);
}

} // namespace

int main() {
    const std::string svg = "<svg width=\"40\" height=\"20\" style=\"display:block;margin-left:10px\"><rect width=\"10\" height=\"10\" fill=\"#00ff00\"/></svg>";
    {
        // A container that does not draw SVG itself (the default answer): el_svg draws the shapes.
        Stub c;
        draw(c, svg);
        check(c.svgs.size() == 1 && c.svgs[0] == "svg", "draw_svg is asked first, with the <svg> element");
        check(c.svg_boxes.size() == 1 && c.svg_boxes[0].x.value() == 10 && c.svg_boxes[0].y.value() == 0 &&
                  c.svg_boxes[0].width.value() == 40 && c.svg_boxes[0].height.value() == 20,
              "draw_svg gets the <svg>'s box in document coordinates");
        check(c.paths.size() == 1 && c.paths[0].fill_color == web_color(0, 255, 0), "draw_svg false: el_svg draws the <rect>");
    }
    {
        // A container that drew it: nothing more of it.
        Stub c;
        c.draws_svg = true;
        draw(c, svg);
        check(c.svgs.size() == 1, "draw_svg true: asked once");
        check(c.paths.empty(), "draw_svg true: el_svg draws none of its shapes");
    }
    {
        // An SVG <style> in a CDATA section, as SVG files and XHTML books write it (W3C cnt-svg-embedded): its rules apply.
        Stub c;
        draw(c, "<svg width=\"40\" height=\"20\"><style><![CDATA[ rect.a { fill: #00ff00 } ]]></style>"
                "<rect class=\"a\" width=\"10\" height=\"10\"/></svg>");
        check(c.paths.size() == 1 && c.paths[0].fill_color == web_color(0, 255, 0),
              "a <style>'s CDATA section is its text: the rect is green");
    }
    std::printf("%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
