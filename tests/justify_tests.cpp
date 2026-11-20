// litehtml without TwidgetCore: text-align: justify spreads a line's free space over the gaps between its characters
// that the CJK rule set allows (cjk::justifiable_gap): between Japanese characters, at spaces and where Latin meets
// Japanese; never inside a Latin word or number, nor inside …… or ――. Horizontal and vertical-rl, where each character
// is drawn and where text selection puts it. The stub container measures 8 px a character and breaks a line at a space
// and after each character from U+2000 unless … or ― follows: its pieces hold several characters (い……, abc漢, 123か――).
#include <litehtml.h>
#include <text_selection.h>

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

// A character drawn: where it starts along the line, and the line's middle across it (laid-out coordinates).
struct Glyph {
    std::string c;
    float x, mid;
};

struct Stub : document_container {
    float w = 300, h = 400;
    std::vector<Glyph> glyphs;
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
            } else if (cp >= 0x2000 && next != U'…' && next != U'―') {
                on_word(s.substr(start, end - start).c_str());
                start = end;
            }
        }
        if (s.size() > start) on_word(s.substr(start).c_str());
    }
    // Each character of a drawn text 8 px after the one before it (vertical text: along the column, upright characters
    // one at a time in a group about their em box, sideways runs as horizontal text).
    void draw_text(uint_ptr, const char* text, uint_ptr, web_color, const position& pos) override {
        const std::string s = text;
        size_t k = 0;
        for (auto [cp, at] : code_points(s))
            glyphs.push_back({utf8(s, at), float(pos.x) + 8.0f * float(k++), float(pos.y) + float(pos.height) / 2});
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

// Where a line's characters go: `line` is its text with a mark at each gap that takes a share of the free space and
// nothing at the others: "|" between Japanese characters takes `gap`, "^" at a word space `space`, "~" where Japanese
// meets Latin `latin`. "あ@0 い@12 ..." (spaces are not drawn: draw_spaces is false).
std::string expected(const std::string& line, float gap, float space = -1, float latin = -1) {
    std::string out;
    float x = 0;
    for (auto [cp, at] : code_points(line)) {
        if (cp == '|' || cp == '^' || cp == '~') {
            x += cp == '|' || space < 0 ? gap : cp == '^' ? space : latin;
            continue;
        }
        if (cp != ' ') out += (out.empty() ? "" : " ") + utf8(line, at) + "@" + std::to_string(int(std::lround(x)));
        x += 8;
    }
    return out;
}

// The characters drawn on the line whose middle is nearest `mid`, as expected() writes them.
std::string drawn_line(const std::vector<Glyph>& glyphs, float mid) {
    std::string out;
    for (const auto& g : glyphs)
        if (std::abs(g.mid - mid) < 1)
            out += (out.empty() ? "" : " ") + g.c + "@" + std::to_string(int(std::lround(g.x)));
    return out;
}

// One justified line of 188 px (18 characters, 144 px, and 44 px for its 11 gaps), then a last line that stays as it
// is:
//   あ い…… う ␠ abc漢 字 123か―― き | defghijkけ こ   (the pieces)
// Spread evenly (line_adjustment::even), each gap takes 4 px. In JLReq 3.8.4's order (jlreq), the two at the word
// space take an eighth em first (2 px), the three where Latin meets Japanese up to half an em (8 px), and the six
// between Japanese characters the 16 px left (8/3 px each, under a quarter em).
const std::string text = "あい……う abc漢字123か――きdefghijkけこ";
const std::string line1 = "あ|い|……|う^ ^abc~漢|字~123~か|――|き";
const std::string line2 = "defghijkけこ";
const float line_length = 188;

void run(bool vertical, line_adjustment adjustment) {
    const bool even = adjustment == line_adjustment::even;
    const std::string mode = std::string(vertical ? "vertical" : "horizontal") + (even ? ", even: " : ", JLReq: ");
    const std::string want1 = even ? expected(line1, 4) : expected(line1, 8.0f / 3, 2, 8);
    Stub c;
    // Vertical text is laid out across and turned: the column is the page's height long.
    if (vertical) c.h = line_length;
    auto doc = document::createFromString(std::string("<html") + (vertical ? " style=\"writing-mode:vertical-rl\"" : "") +
                                              "><body style=\"margin:0;font-size:16px;line-height:20px\"><p style=\"margin:0;text-align:justify\">" +
                                              text + "</p></body></html>",
                                          &c);
    doc->set_line_adjustment(adjustment);
    doc->set_latin_spacing(latin_spacing::none);  // justify's spreading alone (3.2.6 ④: punctuation_tests)
    doc->render(pixel_t(vertical ? 300 : line_length));
    doc->draw(0, 0, 0, nullptr);
    if (c.glyphs.empty()) {
        check(false, mode + "nothing drawn");
        return;
    }
    const float mid1 = c.glyphs.front().mid, mid2 = c.glyphs.back().mid;

    // Gaps between Japanese characters; none inside abc, 123, …… or ――; one each side of the space, and where Latin
    // meets Japanese (c漢, 字1, 3か), in one piece or between two.
    std::string got = drawn_line(c.glyphs, mid1);
    check(got == want1, mode + "the first line spread between its characters: " + got + "\n     want " + want1);
    // The last line as it is.
    got = drawn_line(c.glyphs, mid2);
    check(mid2 != mid1 && got == expected(line2, 0), mode + "the last line is not justified: " + got);

    // Selecting one character highlights where it is drawn: from its start, its 8 px (not the gap after it).
    text_selection ts(*doc);
    position view;
    c.get_viewport(view);
    std::string wrong;
    for (const auto& g : c.glyphs) {
        if (std::abs(g.mid - mid1) >= 1) continue;
        // Laid-out (x along the line, y across it) to the page: vertical text turns, its first column at the right.
        auto page = [&](float x, float y, float& px, float& py) {
            px = vertical ? float(view.width) - y : x;
            py = vertical ? x : y;
        };
        float x0, y0, x1, y1;
        page(g.x + 2, g.mid, x0, y0);
        page(g.x + 6, g.mid, x1, y1);
        const auto rects = ts.rects(ts.position_at(x0, y0), ts.position_at(x1, y1));
        const float start = rects.size() == 1 ? float(vertical ? rects[0].y : rects[0].x) : -1;
        const float length = rects.size() == 1 ? float(vertical ? rects[0].height : rects[0].width) : -1;
        if (std::abs(start - g.x) > 0.01f || std::abs(length - 8) > 0.01f) {
            char at[96];
            std::snprintf(at, sizeof at, "@%g (selected %g+%g)", g.x, start, length);
            wrong += " " + g.c + at;
        }
    }
    check(wrong.empty(), mode + "each character's selection is where it is drawn:" + wrong);
}

} // namespace

int main() {
    for (auto adjustment : {line_adjustment::jlreq, line_adjustment::even}) {
        run(false, adjustment);
        run(true, adjustment);
    }

    // A line that ends with <br> is not justified.
    {
        Stub c;
        auto doc = document::createFromString("<html><body style=\"margin:0;font-size:16px;line-height:20px\"><p style=\"margin:0;text-align:justify\">"
                                              "あい abc<br>うえ</p></body></html>",
                                              &c);
        doc->render(pixel_t(line_length));
        doc->draw(0, 0, 0, nullptr);
        const std::string got = drawn_line(c.glyphs, c.glyphs.empty() ? 0 : c.glyphs.front().mid);
        check(got == expected("あい abc", 0), "a line ending with <br> is not justified: " + got);
    }

    // JLReq 3.5.3 ④: 均等揃え (text-align-last: justify & text-align: justify-all), H & V.
    for (bool vertical : {false, true}) {
        const std::string mode = vertical ? "vertical: " : "horizontal: ";
        auto lay = [&](float len, const std::string& p_style, const std::string& html, const std::string& div_style = "") {
            Stub c;
            if (vertical) c.h = len;
            auto doc = document::createFromString(
                std::string("<html") + (vertical ? " style=\"writing-mode:vertical-rl\"" : "") +
                    "><body style=\"margin:0;font-size:16px;line-height:20px\"><div style=\"" + div_style +
                    "\"><p style=\"margin:0;" + p_style + "\">" + html + "</p></div></body></html>",
                &c);
            doc->set_latin_spacing(latin_spacing::none);
            doc->render(pixel_t(vertical ? 300 : len));
            doc->draw(0, 0, 0, nullptr);
            return c.glyphs;
        };
        // Single-line heading (第一章, 3 chars = 24px in 64px line -> 2 gaps of 20px each: 0, 28, 56).
        {
            const auto gs = lay(64, "text-align-last:justify", "第一章");
            const std::string got = drawn_line(gs, gs.empty() ? 0 : gs.front().mid);
            check(got == "第@0 一@28 章@56", mode + "3.5.3 ④: text-align-last:justify on single line: " + got);
        }
        // Single-line haiku with text-align: justify-all (古池や, 3 chars = 24px in 64px -> 0, 28, 56).
        {
            const auto gs = lay(64, "text-align:justify-all", "古池や");
            const std::string got = drawn_line(gs, gs.empty() ? 0 : gs.front().mid);
            check(got == "古@0 池@28 や@56", mode + "3.5.3 ④: text-align:justify-all on single line: " + got);
        }
        // Respects JLReq 3.1.11 ②: 「第一章」 keeps 「第 and 章」 solid, spreads 第|一|章 evenly.
        {
            const auto gs = lay(80, "text-align:justify-all", "「第一章」");
            // 「 is halved at line start (4px width), 第(8) 一(8) 章(8) 」(8) -> total 36px, 44px free across 2 gaps (22px each).
            // Wait: Stub measures 8px per char, so 「 halved is 4px, 第@4, 一@34, 章@64, 」@72.
            const std::string got = drawn_line(gs, gs.empty() ? 0 : gs.front().mid);
            check(got.find("第@4 ") != std::string::npos && got.find("一@34 ") != std::string::npos &&
                      got.find("章@64 ") != std::string::npos,
                  mode + "3.5.3 ④: justify-all respects 3.1.11 ② next to brackets: " + got);
        }
        // Multi-line paragraph with text-align-last: justify: last line fills full width too.
        {
            const auto gs = lay(64, "text-align:justify;text-align-last:justify", "あいうえおかきくけこ");
            const float mid2 = gs.empty() ? 0 : gs.back().mid;
            const std::string got2 = drawn_line(gs, mid2);
            // 8 chars fit line 1 (64px), 2 chars on line 2 (け@0 こ@56).
            check(got2 == "け@0 こ@56", mode + "3.5.3 ④: text-align-last:justify spreads last line of multi-line paragraph: " + got2);
        }
        // Line before <br> with text-align: justify-all is also justified.
        {
            const auto gs = lay(64, "text-align:justify-all", "古池や<br>蛙飛ぶ");
            const float mid1 = gs.empty() ? 0 : gs.front().mid;
            const float mid2 = gs.empty() ? 0 : gs.back().mid;
            const std::string got1 = drawn_line(gs, mid1);
            const std::string got2 = drawn_line(gs, mid2);
            check(mid1 != mid2 && got1 == "古@0 池@28 や@56" && got2 == "蛙@0 飛@28 ぶ@56",
                  mode + "3.5.3 ④: justify-all justifies before <br> and last line: " + got1 + " | " + got2);
        }
        // Inherited text-align-last: justify from parent <div>.
        {
            const auto gs = lay(64, "", "第一章", "text-align-last:justify");
            const std::string got = drawn_line(gs, gs.empty() ? 0 : gs.front().mid);
            check(got == "第@0 一@28 章@56", mode + "3.5.3 ④: text-align-last:justify inherits from parent: " + got);
        }
        // text-align-last: right / center on a justified paragraph's last line.
        {
            const auto gs_r = lay(64, "text-align:justify;text-align-last:right", "あいうえおかきくけこ");
            const std::string got_r = drawn_line(gs_r, gs_r.empty() ? 0 : gs_r.back().mid);
            check(got_r == "け@48 こ@56", mode + "3.5.3: text-align-last:right aligns last line right: " + got_r);
            const auto gs_c = lay(64, "text-align:justify;text-align-last:center", "あいうえおかきくけこ");
            const std::string got_c = drawn_line(gs_c, gs_c.empty() ? 0 : gs_c.back().mid);
            check(got_c == "け@24 こ@32", mode + "3.5.3: text-align-last:center centers last line: " + got_c);
        }
        // Single character with text-align-last: justify stays at 0 (no NaN / crash).
        {
            const auto gs = lay(64, "text-align-last:justify", "あ");
            const std::string got = drawn_line(gs, gs.empty() ? 0 : gs.front().mid);
            check(got == "あ@0", mode + "3.5.3 ④: single character stays at start: " + got);
        }
    }

    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
