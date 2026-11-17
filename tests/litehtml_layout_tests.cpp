// litehtml's layout from source text, without TwidgetCore: the boxes it gives elements, against a stub container
// whose text is 8 px a byte, whose viewport is 300 x 200 and whose every image is 598 x 800.
#include <litehtml.h>

#include <cstdio>
#include <string>

using namespace litehtml;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) failures++;
}

std::string num(pixel_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", double(float(v)));
    return buf;
}

bool near(pixel_t a, float b) { return float(a) > b - 0.05f && float(a) < b + 0.05f; }

struct Stub : document_container {
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
    void get_image_size(const char*, const char*, size& sz) override { sz = size(598, 800); }
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
};

/// `html` laid out 300 px wide; `head` goes before the body (a <style>).
document::ptr layout(Stub& c, const std::string& body, const std::string& head = "") {
    auto doc = document::createFromString("<html><head>" + head + "</head><body style=\"margin:0\">" + body + "</body></html>", &c);
    doc->render(300);
    return doc;
}

/// The content box of the element `#id`.
position box(const document::ptr& doc, const char* id) {
    auto el = doc->root()->select_one(std::string("#") + id);
    return el ? el->get_placement() : position();
}

} // namespace

int main() {
    {
        // CSS 2.1 10.5: a % height against a containing block whose height is auto computes to auto.
        Stub c;
        auto doc = layout(c, "<div><div id=\"t\" style=\"height:50%\">x</div></div><div><div id=\"r\">x</div></div>");
        check(box(doc, "t").height == box(doc, "r").height,
              "height:50% under auto heights is auto: " + num(box(doc, "t").height) + " (auto gives " + num(box(doc, "r").height) + ")");
        doc = layout(c, "<div><div id=\"t\" style=\"min-height:50%\"></div></div>");
        check(near(box(doc, "t").height, 0), "min-height:50% under auto heights is 0: " + num(box(doc, "t").height));
        doc = layout(c, "<div><div id=\"t\" style=\"max-height:50%\"><div style=\"height:300px\"></div></div></div>");
        check(near(box(doc, "t").height, 300), "max-height:50% under auto heights is none: " + num(box(doc, "t").height));
        doc = layout(c, "<div style=\"display:flex;flex-direction:column\"><div id=\"t\" style=\"height:50%\">x</div></div>"
                        "<div style=\"display:flex;flex-direction:column\"><div id=\"r\">x</div></div>");
        check(box(doc, "t").height == box(doc, "r").height,
              "height:50% of a column flex item under auto heights is auto: " + num(box(doc, "t").height) + " (auto gives " +
                  num(box(doc, "r").height) + ")");
        doc = layout(c, "<div><table id=\"t\" style=\"min-height:50%\"><tr><td>x</td></tr></table></div>"
                        "<div><table id=\"r\"><tr><td>x</td></tr></table></div>");
        check(box(doc, "t").height == box(doc, "r").height,
              "min-height:50% of a table under auto heights is 0: " + num(box(doc, "t").height) + " (auto gives " +
                  num(box(doc, "r").height) + ")");
    }
    {
        // A definite height still resolves one: a length, a chain of % from the root, an absolute box's containing block.
        Stub c;
        auto doc = layout(c, "<div style=\"height:120px\"><div id=\"t\" style=\"height:50%\"></div></div>");
        check(near(box(doc, "t").height, 60), "height:50% of height:120px is 60: " + num(box(doc, "t").height));
        doc = layout(c, "<div id=\"t\" style=\"height:50%\"></div>", "<style>html,body{height:100%}</style>");
        check(near(box(doc, "t").height, 100), "height:50% under html,body{height:100%} is half the viewport's 200: " +
                                                   num(box(doc, "t").height));
        doc = layout(c, "<div style=\"position:relative;height:100px\"><img id=\"t\" style=\"position:absolute;height:50%\"></div>");
        check(near(box(doc, "t").height, 50) && near(box(doc, "t").width, 37.375f), "an absolute img's height:50% of its 100 px containing block is 37.4 x 50: " + num(box(doc, "t").width) + " x " +
                                                  num(box(doc, "t").height));
        doc = layout(c, "<div style=\"position:relative;height:100px\"><table style=\"position:absolute;left:0;right:0;height:50%;border-spacing:0\">"
                        "<tr><td id=\"t\" style=\"padding:0\">x</td></tr></table></div>");
        check(near(box(doc, "t").height, 50), "an absolute table's height:50% of its 100 px containing block gives its cell 50: " +
                                                  num(box(doc, "t").height));
    }
    {
        // The israelsailing cover: an img with height:100% in a div of auto height is its own 598 x 800.
        Stub c;
        auto doc = layout(c, "<div><img id=\"t\" style=\"height:100%\"></div>");
        check(near(box(doc, "t").height, 800) && near(box(doc, "t").width, 598),
              "img height:100% under auto heights is 598 x 800: " + num(box(doc, "t").width) + " x " + num(box(doc, "t").height));
        doc = layout(c, "<div><img id=\"t\" style=\"max-height:100%\"></div>");
        check(near(box(doc, "t").height, 800), "img max-height:100% under auto heights is none: " + num(box(doc, "t").height));
        doc = layout(c, "<div style=\"height:400px\"><img id=\"t\" style=\"max-height:100%\"></div>");
        check(near(box(doc, "t").height, 400) && near(box(doc, "t").width, 299),
              "img max-height:100% of height:400px is 299 x 400: " + num(box(doc, "t").width) + " x " + num(box(doc, "t").height));
    }
    {
        // An img with a set height and an auto width obeys max-width too, and keeps its ratio.
        Stub c;
        auto doc = layout(c, "<img id=\"t\" style=\"height:800px;max-width:300px\">");
        check(near(box(doc, "t").width, 300) && near(box(doc, "t").height, 401.34f),
              "img height:800px;max-width:300px is 300 x 401.34: " + num(box(doc, "t").width) + " x " + num(box(doc, "t").height));
        doc = layout(c, "<img id=\"t\" style=\"height:200px;max-width:250px\">");
        check(near(box(doc, "t").width, 149.5f) && near(box(doc, "t").height, 200),
              "img height:200px;max-width:250px is 149.5 x 200: " + num(box(doc, "t").width) + " x " + num(box(doc, "t").height));
    }
    std::printf("%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
