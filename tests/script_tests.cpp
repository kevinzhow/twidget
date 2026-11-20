// litehtml without TwidgetCore: 添え字 (JLReq 3.1.10 ⑨, 3.7.1), the <sup> and <sub> of a base. Horizontal and
// vertical-rl.
// - No line break between a base and its script (数², H₂O, a note mark after 。), document::set_script_breaking;
//   `css` breaks there as before (and as Chromium does).
// - litehtml::jlreq_scripts_css, a user-agent addition: a script about 60% of its base (JLReq 3.7.1), not `smaller`
//   (83%), and it does not push its line away from the next (line-height 0: the line's pitch stays the paragraph's).
//
// The stub measures each character one em of its font wide; fonts are numbered in the order they are made, and a
// drawn character records its font's size, where it starts along the line and the line's middle across it.
#include <litehtml.h>
#include <litehtml/cjk_rules.h>

#include <cmath>
#include <cstdio>
#include <map>
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

std::string utf8(const std::string& s, size_t at) {
    const unsigned char c = (unsigned char)s[at];
    return s.substr(at, c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4);
}

struct Glyph {
    std::string c;
    float size;   // the font's size
    float along;  // where it starts along the line
    float top;    // its box's top across the line
    float mid;    // its box's middle across the line
};

struct Stub : document_container {
    float w = 400, h = 400;
    std::map<uint_ptr, float> sizes;
    std::vector<Glyph> glyphs;
    uint_ptr create_font(const font_description& d, const document*, font_metrics* fm) override {
        if (fm) {
            fm->font_size = d.size;
            fm->ascent = d.size * 0.8f;
            fm->descent = d.size * 0.2f;
            fm->height = d.size;
            fm->x_height = d.size * 0.5f;
            fm->ch_width = d.size / 2;
            fm->draw_spaces = false;
            fm->super_shift = d.size / 3;
            fm->sub_shift = d.size / 5;
        }
        const uint_ptr id = sizes.size() + 1;
        sizes[id] = float(d.size);
        return id;
    }
    void delete_font(uint_ptr) override {}
    pixel_t text_width(const char* text, uint_ptr font) override {
        return pixel_t(sizes[font] * float(code_points(text).size()));
    }
    void split_text(const char* text, const std::function<void(const char*)>& on_word,
                    const std::function<void(const char*)>& on_space) override {
        const std::string s = text;
        const auto cps = code_points(s);
        size_t start = 0;
        for (size_t i = 0; i < cps.size(); i++) {
            const auto [cp, at] = cps[i];
            const size_t end = i + 1 < cps.size() ? cps[i + 1].second : s.size();
            const char32_t next = i + 1 < cps.size() ? cps[i + 1].first : 0;
            if (cp == ' ') {
                if (at > start) on_word(s.substr(start, at - start).c_str());
                on_space(" ");
                start = end;
            } else if (next && next != U'　' && !cjk::no_break_between(cp, next)) {
                on_word(s.substr(start, end - start).c_str());
                start = end;
            }
        }
        if (s.size() > start) on_word(s.substr(start).c_str());
    }
    void draw_text(uint_ptr, const char* text, uint_ptr font, web_color, const position& pos) override {
        const std::string s = text;
        const float size = sizes[font];
        size_t k = 0;
        for (auto [cp, at] : code_points(s))
            glyphs.push_back({utf8(s, at), size, float(pos.x) + size * float(k++), float(pos.y),
                              float(pos.y) + float(pos.height) / 2});
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

std::string num(float x) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", std::round(x * 10) / 10);
    return b;
}

struct Laid {
    Stub c;
    std::shared_ptr<document> doc;
};

// <p>html</p> in a line (column) `length` px long, 16px text, line-height 20px; with litehtml::jlreq_scripts_css
// after the master stylesheet when `jlreq_ua`.
void lay_out(Laid& l, bool vertical, float length, const std::string& html, script_breaking breaking = script_breaking::keep,
             bool jlreq_ua = true) {
    if (vertical) l.c.h = length;
    const std::string master = std::string(master_css) + (jlreq_ua ? jlreq_scripts_css : "");
    l.doc = document::createFromString(std::string("<html") + (vertical ? " style=\"writing-mode:vertical-rl\"" : "") +
                                           "><body style=\"margin:0;font-size:16px;line-height:20px\"><p style=\"margin:0\">" +
                                           html + "</p></body></html>",
                                       &l.c, master);
    l.doc->set_script_breaking(breaking);
    l.doc->render(pixel_t(vertical ? 400 : length));
    l.doc->draw(0, 0, 0, nullptr);
}

// The characters of each line, lines split by "|": a line starts where a character starts before the one drawn
// before it ends.
std::string line_texts(const std::vector<Glyph>& glyphs) {
    std::string out;
    float end = 0;
    for (const auto& g : glyphs) {
        if (!out.empty() && g.along < end - 0.5f) out += "|";
        out += g.c;
        end = g.along + g.size;
    }
    return out;
}

// Each character and where it starts along its line: "数@0 2@16".
std::string positions(const std::vector<Glyph>& glyphs) {
    std::string out;
    for (const auto& g : glyphs) out += (out.empty() ? "" : " ") + g.c + "@" + num(g.along);
    return out;
}

const Glyph* find(const std::vector<Glyph>& glyphs, const std::string& c) {
    for (const auto& g : glyphs)
        if (g.c == c) return &g;
    return nullptr;
}

// The tops of the lines' 16px glyphs, one per line: "0 20 40".
std::string line_tops(const std::vector<Glyph>& glyphs) {
    std::string out;
    float line = -1;
    for (const auto& g : glyphs)
        if (g.size == 16 && (line < 0 || std::abs(g.mid - line) >= 1)) {
            out += (out.empty() ? "" : " ") + num(g.top);
            line = g.mid;
        }
    return out;
}

void run(bool vertical) {
    const std::string m = vertical ? " (vertical)" : " (horizontal)";

    // 3.1.10 ⑨: 数 fills the line; its ² may not go to the next line alone: 数 goes with it.
    {
        Laid l;
        lay_out(l, vertical, 64, "あいう数<sup>2</sup>えお");
        check(line_texts(l.c.glyphs) == "あいう|数2えお", "3.1.10 ⑨: 数² is not split" + m + ": " + line_texts(l.c.glyphs));
    }
    {
        Laid l;
        lay_out(l, vertical, 64, "あいう数<sup>2</sup>えお", script_breaking::css);
        check(line_texts(l.c.glyphs) == "あいう数|2えお", "script_breaking::css: 数|² as before" + m + ": " + line_texts(l.c.glyphs));
    }
    {
        Laid l;
        lay_out(l, vertical, 64, "あいう数<sub>2</sub>えお");
        check(line_texts(l.c.glyphs) == "あいう|数2えお", "3.1.10 ⑨: a subscript is not split from its base" + m + ": " +
                                                              line_texts(l.c.glyphs));
    }
    // H₂O: neither after H nor after ₂.
    {
        Laid l;
        lay_out(l, vertical, 64, "あいうH<sub>2</sub>Oえ");
        check(line_texts(l.c.glyphs) == "あいう|H2Oえ", "3.1.10 ⑨: H₂O is not split" + m + ": " + line_texts(l.c.glyphs));
    }
    // A note mark after 。 stays with it (。 may not start a line, so う goes along).
    {
        Laid l;
        lay_out(l, vertical, 64, "あいう。<sup>注</sup>えお");
        check(line_texts(l.c.glyphs).find("。注") != std::string::npos,
              "3.1.10 ⑨: a note mark after 。 is not split from it" + m + ": " + line_texts(l.c.glyphs));
    }
    // After a space, a script may start a line.
    {
        Laid l;
        lay_out(l, vertical, 64, "abc <sup>1</sup>def");
        check(line_texts(l.c.glyphs) == "abc|1def", "a script after a space may start the line" + m + ": " + line_texts(l.c.glyphs));
    }

    // 3.7.1: a script is set solid with its base and what follows: no quarter em between Japanese and a Latin digit
    // there (3.2.6 ④ is for Latin text, not for 添え字); a Latin word after kana still gets it.
    {
        Laid l;
        lay_out(l, vertical, 400, "数<sup>2</sup>く、数<sub>3</sub>と");
        check(positions(l.c.glyphs) == "数@0 2@16 く@25.6 、@41.6 数@57.6 3@73.6 と@83.2",
              "3.7.1: 数²く solid, no quarter em next to the script" + m + ": " + positions(l.c.glyphs));
        // (In vertical text a lone digit stands upright in one em, tate-chu-yoko, solid anyway.)
        Laid w;
        lay_out(w, vertical, 400, "数<sup>2</sup>く、数<sub>3</sub>と", script_breaking::css);
        check(vertical || positions(w.c.glyphs) != positions(l.c.glyphs), "script_breaking::css: the quarter em next to the script as before" + m +
                                                                  ": " + positions(w.c.glyphs));
        Laid t;
        lay_out(t, vertical, 400, "かなWord");
        check(positions(t.c.glyphs) == "か@0 な@16 W@36 o@52 r@68 d@84", "3.2.6 ④: kana before a Latin word keep the quarter em" + m +
                                                                            ": " + positions(t.c.glyphs));
    }

    // 3.7.1: about 60% of the base.
    {
        Laid l;
        lay_out(l, vertical, 400, "数<sup>2</sup>と数<sub>3</sub>");
        const Glyph* two = find(l.c.glyphs, "2");
        const Glyph* three = find(l.c.glyphs, "3");
        check(two && num(two->size) == "9.6", "3.7.1: a superscript is 60% of its base" + m + ": " + (two ? num(two->size) : "none"));
        check(three && num(three->size) == "9.6", "3.7.1: a subscript is 60% of its base" + m + ": " + (three ? num(three->size) : "none"));
    }
    {
        Laid l;
        lay_out(l, vertical, 400, "数<sup>2</sup>", script_breaking::keep, false);
        const Glyph* two = find(l.c.glyphs, "2");
        check(two && num(two->size) == "13.3", "without jlreq_scripts_css: smaller (83%) as before" + m + ": " +
                                                   (two ? num(two->size) : "none"));
    }
    // A book's own size for its scripts wins.
    {
        Laid l;
        lay_out(l, vertical, 400, "数<sup style=\"font-size:12px\">2</sup>");
        const Glyph* two = find(l.c.glyphs, "2");
        check(two && num(two->size) == "12", "a book's own script size wins" + m + ": " + (two ? num(two->size) : "none"));
    }

    // 3.7.1 / 3.2.2: a line with a script keeps the paragraph's line pitch (20px; each line's 16px glyphs 2px below its
    // top), superscript or subscript.
    for (const char* tag : {"sup", "sub"}) {
        Laid l;
        lay_out(l, vertical, 64, std::string("あいうえ数<") + tag + ">2</" + tag + ">かきくけこさしすせ");
        check(line_tops(l.c.glyphs) == "2 22 42 62", std::string("3.7.1: a line with <") + tag + "> keeps the pitch" + m + ": " +
                                                      line_tops(l.c.glyphs));
    }
    // Its ink stays in its line's half: a superscript above the base's middle, a subscript below.
    {
        Laid l;
        lay_out(l, vertical, 400, "数<sup>2</sup>数<sub>3</sub>");
        const Glyph* base = find(l.c.glyphs, "数");
        const Glyph* two = find(l.c.glyphs, "2");
        const Glyph* three = find(l.c.glyphs, "3");
        check(base && two && three && two->mid < base->mid && three->mid > base->mid,
              "3.7.1: ² above the base's middle, ₃ below" + m);
    }
}

}  // namespace

int main() {
    for (bool vertical : {false, true}) run(vertical);
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
