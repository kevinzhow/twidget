// litehtml's layout from source text, without TwidgetCore: the boxes it gives elements, against a stub container
// whose text is 8 px a byte, whose viewport is 300 x 200 and whose every image is 598 x 800.
#include <litehtml.h>

#include <gumbo.h>

#include <cstdio>
#include <string>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

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

/// The box of each <b>'s text as "x,y w" in order, and the block's height: the line boxes the text makes.
std::string words(const document::ptr& doc) {
    std::string out;
    for (const auto& el : doc->root()->select_all("b")) {
        const position p = el->children().front()->get_placement();
        out += num(p.x) + "," + num(p.y) + " " + num(p.width) + "; ";
    }
    return out + "h=" + num(box(doc, "t").height);
}

/// The elements under `el` that are white space (el_space).
size_t spaces(const element::ptr& el) {
    size_t n = el->is_space() ? 1 : 0;
    for (const auto& child : el->children()) n += spaces(child);
    return n;
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
    {
        // White space between elements under each white-space value: the same line boxes as one element per character
        // gave (130 px wide: 16 characters of 8 px).
        const std::string text = "<b>aa</b>   \n    <b>bb</b>\t<b>cc</b>\n\n  <b>dd</b>  <b>eeeeeeeeee</b>          <b>ff</b> \r\n "
                                 "<b>gg</b>\t\t<b>hh</b> <b>\xe6\x97\xa5</b>\n  <b>\xe6\x9c\xac</b>";
        const char* modes[] = {"normal", "nowrap", "pre-line", "pre", "pre-wrap", "break-spaces"};
        const char* expected[] = {
            "0.00,0.00 16.00; 24.00,0.00 16.00; 48.00,0.00 16.00; 72.00,0.00 16.00; 0.00,16.00 80.00; 88.00,16.00 16.00; 112.00,16.00 16.00; 0.00,32.00 16.00; 24.00,32.00 24.00; 56.00,32.00 24.00; h=48.00",
            "0.00,0.00 16.00; 24.00,0.00 16.00; 48.00,0.00 16.00; 72.00,0.00 16.00; 96.00,0.00 80.00; 184.00,0.00 16.00; 208.00,0.00 16.00; 232.00,0.00 16.00; 256.00,0.00 24.00; 288.00,0.00 24.00; h=16.00",
            "0.00,0.00 16.00; 24.00,0.00 16.00; 48.00,0.00 16.00; 0.00,16.00 16.00; 24.00,16.00 80.00; 112.00,16.00 16.00; 0.00,32.00 16.00; 24.00,32.00 16.00; 48.00,32.00 24.00; 0.00,48.00 24.00; h=64.00",
            "0.00,0.00 16.00; 32.00,16.00 16.00; 80.00,16.00 16.00; 16.00,48.00 16.00; 48.00,48.00 80.00; 208.00,48.00 16.00; 8.00,64.00 16.00; 88.00,64.00 16.00; 112.00,64.00 24.00; 16.00,80.00 24.00; h=96.00",
            // pre-wrap as main lays it out since 3a3ca6c: eeeeeeeeee, followed by kept spaces, goes to the next line.
            "0.00,0.00 16.00; 32.00,16.00 16.00; 80.00,16.00 16.00; 16.00,48.00 16.00; 0.00,64.00 80.00; 0.00,80.00 16.00; 8.00,96.00 16.00; 88.00,96.00 16.00; 0.00,112.00 24.00; 16.00,128.00 24.00; h=144.00",
            "0.00,0.00 16.00; 24.00,0.00 16.00; 48.00,0.00 16.00; 72.00,0.00 16.00; 0.00,16.00 80.00; 88.00,16.00 16.00; 112.00,16.00 16.00; 0.00,32.00 16.00; 24.00,32.00 24.00; 56.00,32.00 24.00; h=48.00"};
        for (int m = 0; m < 6; m++) {
            Stub c;
            auto doc = layout(c, std::string("<div id=\"t\" style=\"width:130px;white-space:") + modes[m] + "\">" + text + "</div>");
            const std::string got = words(doc);
            check(got == expected[m], std::string("white-space:") + modes[m] + ": " + got);
        }
        // The same when a patch changes white-space (update_from_string restyles in place).
        auto html = [&](const char* mode) {
            return std::string("<html><head></head><body style=\"margin:0\"><div id=\"t\" style=\"width:130px;white-space:") + mode +
                   "\">" + text + "</div></body></html>";
        };
        for (int from : {0, 3}) {
            const int to = 3 - from;
            Stub c;
            auto doc = document::createUpdatableFromString(html(modes[from]), &c, master_css);
            doc->render(300);
            bool changed = false;
            const bool patched = doc->update_from_string(html(modes[to]), &changed);
            doc->render(300);
            const std::string got = words(doc);
            check(patched && changed && got == expected[to],
                  std::string("white-space:") + modes[from] + " patched to " + modes[to] + ": " + got);
        }
    }
    {
        // 100 KB of indentation around paragraphs, as a pretty-printed book has it: one element for each run of white
        // space, not one for each character.
        std::string body = "<div>", bare = "<div>";
        for (int i = 0; i < 100; i++) {
            body += "\n" + std::string(1000, ' ') + "<p>w</p>";
            bare += "<p>w</p>";
        }
        Stub c;
        auto doc = layout(c, body + "\n</div>");
        const size_t n = spaces(doc->root());
        check(n <= 101, "100 runs of 1001 white space characters make " + std::to_string(n) + " elements (at most 101)");
        auto without = layout(c, bare + "</div>");
        check(doc->height() == without->height(), "the paragraphs are laid out as without the indentation: " + num(doc->height()) +
                                                      " (" + num(without->height()) + " without)");
    }
    {
        // 1 MB of bytes that are not HTML (a disk image a book names as its chapter): gumbo records no parse errors,
        // which nothing reads, so the parse takes about as much memory as the text it keeps.
        std::string bytes(1 << 20, '\0');
        uint32_t seed = 1;
        for (auto& b : bytes) {
            seed = seed * 1664525u + 1013904223u;
            b = char(seed >> 24);
        }
#if defined(__GLIBC__)
        const size_t before = mallinfo2().uordblks;
#endif
        GumboOutput* out = gumbo_parse_with_options(&kGumboDefaultOptions, bytes.data(), bytes.size());
#if defined(__GLIBC__)
        const size_t heap = mallinfo2().uordblks - before;
        check(heap < size_t(8) << 20, "the parse of 1 MB of binary holds " + std::to_string(heap >> 20) + " MB (under 8)");
#endif
        check(out->errors.length == 0, "the parse of 1 MB of binary records " + std::to_string(out->errors.length) + " errors (0)");
        gumbo_destroy_output(&kGumboDefaultOptions, out);
    }
    std::printf("%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
