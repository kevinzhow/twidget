// litehtml without TwidgetCore: punctuation compression by the CJK rule set (cjk::compression), as stone_engine's
// "stone" mode sets it. An opening bracket at a line's start, or after another punctuation mark, gives up the first half
// of its em; a closing bracket or 、。 before a closing mark, 、。 or a middle dot gives up its second half, and so does
// one ending a wrapped line (not one before <br>, nor on a paragraph's last line). The line breaks with these trims
// counted. Horizontal and vertical-rl, where each character's ink is and where text selection puts it.
//
// The stub measures 16 px a character (font-size 16px) and breaks a line between any two characters the rule set does
// not keep together, never before U+3000. A font whose description has halt (vhal in vertical text) is the half-width
// font: an opening bracket drawn with it has its ink in the first half of where it is drawn, as Noto CJK's halt and vhal
// move it; drawn with the full-width font, its ink is in the second half. With `half_forms`, the stub's fonts have
// half-width forms (has_half_width_form), and a trimmed character must be drawn with the half-width font; without, it
// must be drawn full width, moved back by the half it gives up.
#include <litehtml.h>
#include <litehtml/cjk_rules.h>
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

const uint_ptr full_font = 1, half_font = 2;

// A character drawn: where its ink starts along the line, whether with the half-width font, and the line's middle
// across it (laid-out coordinates).
struct Glyph {
    std::string c;
    float ink;
    bool half;
    float mid;
};

struct Stub : document_container {
    float w = 400, h = 400;
    bool half_forms = false;
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
        return d.features.find("halt") != std::string::npos || d.features.find("vhal") != std::string::npos ? half_font : full_font;
    }
    void delete_font(uint_ptr) override {}
    pixel_t text_width(const char* text, uint_ptr) override { return pixel_t(16.0f * float(code_points(text).size())); }
    bool has_half_width_form(const char*, uint_ptr, bool) override { return half_forms; }
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
    // Each character of a drawn text 16 px after the one before it; an opening bracket's ink in the second half of its
    // em, or the first with the half-width font.
    void draw_text(uint_ptr, const char* text, uint_ptr font, web_color, const position& pos) override {
        const std::string s = text;
        size_t k = 0;
        for (auto [cp, at] : code_points(s)) {
            const bool opening = cjk::classify(cp) & cjk::opening;
            const float x = float(pos.x) + 16.0f * float(k++);
            glyphs.push_back({utf8(s, at), opening && font != half_font ? x + 8 : x, font == half_font,
                              float(pos.y) + float(pos.height) / 2});
        }
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

// Each line's characters as "c@ink", a trimmed one drawn with the half-width font as "c*@ink"; lines split by " | ".
std::string drawn(const std::vector<Glyph>& glyphs, bool marks) {
    std::string out;
    float mid = glyphs.empty() ? 0 : glyphs.front().mid;
    for (const auto& g : glyphs) {
        if (std::abs(g.mid - mid) >= 1) out += " |";
        mid = g.mid;
        out += (out.empty() ? "" : " ") + g.c + (marks && g.half ? "*" : "") + "@" + num(g.ink);
    }
    return out;
}

struct Laid {
    Stub c;
    std::shared_ptr<document> doc;
};

// <p>html</p> in a line (column) `length` px long, text-align `align`.
void lay_out(Laid& l, bool vertical, bool half_forms, float length, const std::string& html, const char* align = "justify") {
    l.c.half_forms = half_forms;
    if (vertical) l.c.h = length;
    l.doc = document::createFromString(std::string("<html") + (vertical ? " style=\"writing-mode:vertical-rl\"" : "") +
                                           "><body style=\"margin:0;font-size:16px;line-height:20px\"><p style=\"margin:0;text-align:" +
                                           align + "\">" + html + "</p></body></html>",
                                       &l.c);
    l.doc->render(pixel_t(vertical ? 400 : length));
    l.doc->draw(0, 0, 0, nullptr);
}

// `body` (paragraphs of their own) in a line (column) `length` px long.
void lay_out_body(Laid& l, bool vertical, bool half_forms, float length, const std::string& body,
                  paragraph_brackets scheme = paragraph_brackets::full_em) {
    l.c.half_forms = half_forms;
    if (vertical) l.c.h = length;
    l.doc = document::createFromString(std::string("<html") + (vertical ? " style=\"writing-mode:vertical-rl\"" : "") +
                                           "><body style=\"margin:0;font-size:16px;line-height:20px\">" + body + "</body></html>",
                                       &l.c);
    l.doc->set_paragraph_brackets(scheme);
    l.doc->render(pixel_t(vertical ? 400 : length));
    l.doc->draw(0, 0, 0, nullptr);
}

// Where selecting the `n`th drawn character (from `from` px into its ink, 2 px before the end of `ink_length`)
// highlights along the line: "start+length".
std::string selected(Laid& l, bool vertical, size_t n, float ink_length = 8, float from = 2) {
    if (n >= l.c.glyphs.size()) return "none";
    const Glyph& g = l.c.glyphs[n];
    text_selection ts(*l.doc);
    position view;
    l.c.get_viewport(view);
    auto page = [&](float x, float y, float& px, float& py) {
        px = vertical ? float(view.width) - y : x;
        py = vertical ? x : y;
    };
    float x0, y0, x1, y1;
    page(g.ink + from, g.mid, x0, y0);
    page(g.ink + ink_length - 2, g.mid, x1, y1);
    const auto rects = ts.rects(ts.position_at(x0, y0), ts.position_at(x1, y1));
    if (rects.size() != 1) return "rects " + std::to_string(rects.size());
    return num(float(vertical ? rects[0].y : rects[0].x)) + "+" + num(float(vertical ? rects[0].height : rects[0].width));
}

void run(bool vertical, bool half_forms) {
    const std::string mode = std::string(vertical ? "vertical" : "horizontal") + (half_forms ? ", halt/vhal: " : ", no halt/vhal: ");
    auto expect = [&](float length, const std::string& html, const std::string& want, const std::string& what,
                      const char* align = "justify") {
        Laid l;
        lay_out(l, vertical, half_forms, length, html, align);
        const std::string got = drawn(l.c.glyphs, half_forms);
        std::string w = want;
        if (!half_forms)
            for (size_t at; (at = w.find('*')) != std::string::npos;) w.erase(at, 1);
        check(got == w, mode + what + ": " + got + "\n     want " + w);
    };

    // Kevin's page: 「 at a column's top, 。「, and ？ followed by U+3000 (kept full width, as stone keeps it).
    expect(400, "「ええと。あなた", "「*@0 え@8 え@24 と@40 。@56 あ@72 な@88 た@104", "an opening bracket at a line's start");
    expect(400, "詰め寄った。「主人", "詰@0 め@16 寄@32 っ@48 た@64 。@80 「*@96 主@104 人@120",
           "an opening bracket after punctuation (。「)");
    expect(400, "ですか？　と言う", "で@0 す@16 か@32 ？@48 　@64 と@80 言@96 う@112", "？ and U+3000 stay full width");
    // The other pairs: ）「 halves 「; 」。 and 」・ halve 」; ・ itself is not trimmed (stone's "half" mode only).
    expect(400, "あ）「い", "あ@0 ）@16 「*@32 い@40", "an opening bracket after a closing one");
    expect(400, "あ」。い", "あ@0 」*@16 。@24 い@40", "a closing bracket before 。");
    expect(400, "あ」・い", "あ@0 」*@16 ・@24 い@40", "a closing bracket before a middle dot");
    expect(400, "あ。」い", "あ@0 。*@16 」@24 い@40", "。 before a closing bracket");
    expect(400, "あ」「い", "あ@0 」@16 「*@32 い@40", "」「: the opening one halves");
    expect(400, "あ<span>。</span>「い", "あ@0 。@16 「*@32 い@40", "a pair across elements");
    // A wrapped line ending with 。 gives up its second half: justified, its ink ends at the line's end and the other
    // characters take the 8 px between them (5 gaps). Before <br> and on the last line, 。 stays full width.
    expect(96, "あいうえお。かきく", "あ@0 い@17.6 う@35.2 え@52.8 お@70.4 。*@88 | か@0 き@16 く@32",
           "。 ending a wrapped line");
    expect(96, "あいうえお。<br>かき。", "あ@0 い@16 う@32 え@48 お@64 。@80 | か@0 き@16 。@32",
           "。 before <br> and ending the paragraph");
    expect(96, "あいうえ」<br>か", "あ@0 い@16 う@32 え@48 」@64 | か@0", "」 before <br>", "left");
    // The line breaks with the trims counted: each of these fits one line only trimmed.
    expect(88, "「あいうえお", "「*@0 あ@8 い@24 う@40 え@56 お@72", "a line that fits with its first 「 halved");
    expect(104, "あいう」。えお", "あ@0 い@16 う@32 」*@48 。@56 え@72 お@88", "a line that fits with 」。 halved");
    expect(104, "あ。「いうえお", "あ@0 。@16 「*@32 い@40 う@56 え@72 お@88", "a line that fits with 。「 halved");
    // Every line's first opening bracket, the wrapped ones too.
    expect(64, "あいうえ「かき", "あ@0 い@16 う@32 え@48 | 「*@0 か@8 き@24", "「 starting a wrapped line");

    // JLReq 3.1.5 ①: an opening bracket that starts a paragraph has a full em before its ink (改行行頭 全角アキ), one
    // that starts a wrapped line none (折り返し行頭 天ツキ). The paragraph's indent is the book's: its own text-indent
    // of 1em takes the bracket's first half (as before); a paragraph with none, in a book whose paragraphs are indented
    // (a U+3000 or text-indent on its sibling paragraphs, or on its lines after <br>), gets the em: the bracket full
    // width after half an em. Paragraphs in a book that indents none stay as they were.
    auto expect_body = [&](float length, const std::string& body, const std::string& want, const std::string& what,
                           paragraph_brackets scheme = paragraph_brackets::full_em) {
        Laid l;
        lay_out_body(l, vertical, half_forms, length, body, scheme);
        const std::string got = drawn(l.c.glyphs, half_forms);
        std::string w = want;
        if (!half_forms)
            for (size_t at; (at = w.find('*')) != std::string::npos;) w.erase(at, 1);
        check(got == w, mode + what + ": " + got + "\n     want " + w);
    };
    const std::string p0 = "<p style=\"margin:0\">", p1 = "<p style=\"margin:0;text-indent:1em\">", e = "</p>";
    expect_body(80, p0 + "　あいうえ" + e + p0 + "「かき」くけこさしす" + e,
                "　@0 あ@16 い@32 う@48 え@64 | 「@16 か@24 き@40 」*@56 | く@0 け@16 こ@32 さ@48 し@64 | す@0",
                "3.1.5 ①: a paragraph that starts with 「 after one indented with U+3000: 「 full width after half an em");
    expect_body(80, p0 + "「かき」くけこさ「しす" + e + p0 + "　あ" + e,
                "「@16 か@24 き@40 」*@56 | く@0 け@16 こ@32 さ@48 | 「*@0 し@8 す@24 | 　@0 あ@16",
                "3.1.5 ①: the same paragraph's wrapped line starts with 「 flush (天ツキ)");
    expect_body(400, p1 + "あい" + e + "<p style=\"margin:0;text-indent:0\">「か」き</p>",
                "あ@16 い@32 | 「@16 か@24 」@40 き@56",
                "3.1.5 ①: a text-indent:0 paragraph that starts with 「 between text-indent:1em ones (草枕's conversation)");
    expect_body(400, p1 + "「か」き" + e, "「*@16 か@24 」@40 き@56",
                "3.1.5 ①: a paragraph's own text-indent:1em takes the first half of its 「, as before");
    expect_body(400, p0 + "あい" + e + p0 + "「か」き" + e, "あ@0 い@16 | 「*@0 か@8 」@24 き@40",
                "a book that indents no paragraph: 「 at a paragraph's start as at a line's start, as before");
    expect_body(400, "<p style=\"margin:0\">　あい<br/>「か」き<br/>　う</p>", "　@0 あ@16 い@32 | 「@16 か@24 」@40 き@56 | 　@0 う@16",
                "3.1.5 ①: lines after <br> are paragraphs (毒姫): 「 after a <br> among U+3000 lines");
    expect_body(400, "<h1 style=\"margin:0;font-size:16px\">「題」</h1>" + p0 + "　あ" + e, "「*@0 題@8 」@24 | 　@0 あ@16",
                "a heading is not one of the paragraphs: its 「 as before");
    expect_body(400, p0 + "　あ" + e + "<p style=\"margin:0;text-align:center\">「か」</p>", "　@0 あ@16 | 「*@180 か@188 」@204",
                "a centred paragraph's 「 as before");
    // JLReq 3.1.5 ③ (document::set_paragraph_brackets(half_em), as novels set dialogue): the same paragraphs' bracket
    // keeps its full width and no more (二分アキ, its ink half an em in); a wrapped line's is still 天ツキ, a paragraph's
    // own text-indent:1em still takes its first half, a book that indents nothing is still flush.
    const auto three = paragraph_brackets::half_em;
    expect_body(80, p0 + "　あいうえ" + e + p0 + "「かき」くけこさしす" + e,
                "　@0 あ@16 い@32 う@48 え@64 | 「@8 か@16 き@32 」@48 く@64 | け@0 こ@16 さ@32 し@48 す@64",
                "3.1.5 ③: a paragraph that starts with 「 after one indented with U+3000: 「 full width (二分アキ)", three);
    expect_body(80, p0 + "「かき」くけこさし「すせ" + e + p0 + "　あ" + e,
                "「@8 か@16 き@32 」@48 く@64 | け@0 こ@16 さ@32 し@48 | 「*@0 す@8 せ@24 | 　@0 あ@16",
                "3.1.5 ③: the paragraph's wrapped line still starts with 「 flush (天ツキ)", three);
    expect_body(400, p1 + "あい" + e + "<p style=\"margin:0;text-indent:0\">「か」き</p>", "あ@16 い@32 | 「@8 か@16 」@32 き@48",
                "3.1.5 ③: 草枕's text-indent:0 conversation among text-indent:1em paragraphs", three);
    expect_body(400, p1 + "「か」き" + e, "「*@16 か@24 」@40 き@56", "3.1.5 ③: a paragraph's own text-indent:1em as in ①", three);
    expect_body(400, p0 + "あい" + e + p0 + "「か」き" + e, "あ@0 い@16 | 「*@0 か@8 」@24 き@40",
                "3.1.5 ③: a book that indents no paragraph as before", three);
    expect_body(400, "<p style=\"margin:0\">　あい<br/>「か」き<br/>　う</p>", "　@0 あ@16 い@32 | 「@8 か@16 」@32 き@48 | 　@0 う@16",
                "3.1.5 ③: lines after <br> (毒姫)", three);
    {
        Laid l;
        lay_out_body(l, vertical, half_forms, 400, p0 + "　あ" + e + p0 + "「か」" + e);
        // its em box, from 2 px into it: its ink is in its second half
        check(selected(l, vertical, 2, 8, -6) == "8+16", mode + "selecting a paragraph's first 「 (full width): " +
                                                               selected(l, vertical, 2, 8, -6));
        check(selected(l, vertical, 3, 16) == "24+16", mode + "selecting か after it: " + selected(l, vertical, 3, 16));
    }

    // Selecting a trimmed character highlights its half; the next one starts where its ink does.
    {
        Laid l;
        lay_out(l, vertical, half_forms, 400, "「ええと。");
        check(selected(l, vertical, 0) == "0+8", mode + "selecting 「 at the line's start: " + selected(l, vertical, 0));
        check(selected(l, vertical, 1, 16) == "8+16", mode + "selecting え after it: " + selected(l, vertical, 1, 16));
    }
    {
        Laid l;
        lay_out(l, vertical, half_forms, 400, "た。「主");
        check(selected(l, vertical, 2) == "32+8", mode + "selecting 「 after 。: " + selected(l, vertical, 2));
        check(selected(l, vertical, 3, 16) == "40+16", mode + "selecting 主 after it: " + selected(l, vertical, 3, 16));
    }
}

} // namespace

int main() {
    for (bool vertical : {false, true})
        for (bool half_forms : {true, false}) run(vertical, half_forms);
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
