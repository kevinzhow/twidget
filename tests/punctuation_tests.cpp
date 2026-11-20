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
void lay_out(Laid& l, bool vertical, bool half_forms, float length, const std::string& html, const char* align = "justify",
             line_adjustment adjustment = line_adjustment::jlreq, latin_spacing spacing = latin_spacing::quarter,
             dividing_space dividing = dividing_space::full_em) {
    l.c.half_forms = half_forms;
    if (vertical) l.c.h = length;
    l.doc = document::createFromString(std::string("<html") + (vertical ? " style=\"writing-mode:vertical-rl\"" : "") +
                                           "><body style=\"margin:0;font-size:16px;line-height:20px\"><p style=\"margin:0;text-align:" +
                                           align + "\">" + html + "</p></body></html>",
                                       &l.c);
    l.doc->set_line_adjustment(adjustment);
    l.doc->set_latin_spacing(spacing);
    l.doc->set_dividing_space(dividing);
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

// The characters of each line, lines split by "|".
std::string line_texts(const std::vector<Glyph>& glyphs) {
    std::string out;
    float mid = glyphs.empty() ? 0 : glyphs.front().mid;
    for (const auto& g : glyphs) {
        if (std::abs(g.mid - mid) >= 1) out += "|";
        mid = g.mid;
        out += g.c;
    }
    return out;
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
    // characters take the 8 px between them (the 4 gaps not next to 。, JLReq 3.1.11 ②). Before <br> and on the last line, 。 stays full width.
    expect(96, "あいうえお。かきく", "あ@0 い@18 う@36 え@54 お@72 。*@88 | か@0 き@16 く@32",
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
    // (」 gives up its half em to take く in, JLReq 3.8.2.)
    expect_body(80, p0 + "　あいうえ" + e + p0 + "「かき」くけこさしす" + e,
                "　@0 あ@16 い@32 う@48 え@64 | 「@16 か@24 き@40 」*@56 く@64 | け@0 こ@16 さ@32 し@48 す@64",
                "3.1.5 ①: a paragraph that starts with 「 after one indented with U+3000: 「 full width after half an em");
    expect_body(80, p0 + "「かき」くけこさし「すせ" + e + p0 + "　あ" + e,
                "「@16 か@24 き@40 」*@56 く@64 | け@0 こ@16 さ@32 し@48 | 「*@0 す@8 せ@24 | 　@0 あ@16",
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
    expect_body(400, p1 + "「か」き" + e, "「@8 か@16 」@32 き@48",
                "3.1.5 ③: a paragraph's own text-indent:1em sets 二分アキ", three);
    expect_body(80, p1 + "あいうえ" + e + p1 + "「かき」くけこさし「すせ" + e,
                "あ@16 い@32 う@48 え@64 | 「@8 か@16 き@32 」@48 く@64 | け@0 こ@16 さ@32 し@48 | 「*@0 す@8 せ@24",
                "3.1.5 ③: CSS text-indent:1em paragraph sets 二分アキ, wrapped 「 stays 天ツキ", three);
    expect_body(400, p1 + "<span>「か」</span>き" + e, "「@8 か@16 」@32 き@48",
                "3.1.5 ③: CSS text-indent:1em with <span> around 「", three);
    expect_body(400, "<p style=\"margin:0;text-indent:2em\">「か」き</p>", "「*@32 か@40 」@56 き@72",
                "3.1.5 ③: deep text-indent:2em stays untouched", three);
    expect_body(400, p0 + "あい" + e + p0 + "「か」き" + e, "あ@0 い@16 | 「*@0 か@8 」@24 き@40",
                "3.1.5 ③: a book that indents no paragraph as before", three);
    expect_body(400, "<p style=\"margin:0\">　あい<br/>「か」き<br/>　う</p>", "　@0 あ@16 い@32 | 「@8 か@16 」@32 き@48 | 　@0 う@16",
                "3.1.5 ③: lines after <br> (毒姫)", three);
    // P0-2: long dialogue sequences (>16 consecutive dialogue paragraphs), <div><p> wrappers, line-1 U+3000 before <br>,
    // and <br/> followed by <span> must not break indentation or falsely justify the line before <br/>.
    for (auto scheme : {paragraph_brackets::full_em, paragraph_brackets::half_em}) {
        const std::string sname = scheme == paragraph_brackets::full_em ? "3.1.5 ①: " : "3.1.5 ③: ";
        const std::string bpos = scheme == paragraph_brackets::full_em ? "「@16 か@24 」@40" : "「@8 か@16 」@32";
        std::string many = p0 + "　あ" + e;
        std::string want_many = "　@0 あ@16";
        for (int k = 0; k < 19; k++) {
            many += p0 + "「か」" + e;
            want_many += " | " + bpos;
        }
        many += p0 + "　い" + e;
        want_many += " | 　@0 い@16";
        expect_body(400, many, want_many, sname + "19 consecutive dialogue paragraphs all keep bracket indent", scheme);
        expect_body(400, "<div>" + p0 + "　あ" + e + "</div><div>" + p0 + "「か」" + e + "</div>",
                    "　@0 あ@16 | " + bpos, sname + "<div><p> wrappers share paragraph indentation", scheme);
        expect_body(400, p0 + "　あい<br/>「か」き" + e,
                    "　@0 あ@16 い@32 | " + bpos + (scheme == paragraph_brackets::full_em ? " き@56" : " き@48"),
                    sname + "<br/> after line-1 U+3000 indents following 「", scheme);
        expect_body(200, "<p style=\"margin:0;text-align:justify\">　あいう<br/>「かき」く<br/><span>「さし」</span>す</p>",
                    scheme == paragraph_brackets::full_em
                        ? "　@0 あ@16 い@32 う@48 | 「@16 か@24 き@40 」@56 く@72 | 「@16 さ@24 し@40 」@56 す@72"
                        : "　@0 あ@16 い@32 う@48 | 「@8 か@16 き@32 」@48 く@64 | 「@8 さ@16 し@32 」@48 す@64",
                    sname + "<br/> followed by <span> does not justify line before <br/>", scheme);
    }
    expect(160, "あいう。<br/><span>か</span>", "あ@0 い@16 う@32 。@48 | か@0",
           "3.1.9: 。 before <br/><span> stays full width and line is not justified");
    {
        Laid l;
        lay_out_body(l, vertical, half_forms, 400, p0 + "　あ" + e + p0 + "「か」" + e);
        // its em box, from 2 px into it: its ink is in its second half
        check(selected(l, vertical, 2, 8, -6) == "8+16", mode + "selecting a paragraph's first 「 (full width): " +
                                                               selected(l, vertical, 2, 8, -6));
        check(selected(l, vertical, 3, 16) == "24+16", mode + "selecting か after it: " + selected(l, vertical, 3, 16));
    }

    // JLReq 3.8 line adjustment (document::set_line_adjustment, jlreq by default). A line over-full by no more than its
    // punctuation's adjustable space takes the character in, squeezing that space (追込み, 3.8.2), before it pushes
    // anything on to the next line (追出し): first the half em after a closing bracket or 、。 ending the line, then the
    // quarters around a middle dot ending it, then those around middle dots inside it, then the half em before opening
    // brackets and after closing brackets and 、 inside it (3.8.3), each step evenly; the half em after 。 inside a line
    // stays. Expanding, a line puts no space before or after brackets, 、。 and middle dots (3.1.11 ②), and widens first
    // Latin word spaces (an eighth em each side at most), then where Japanese meets Latin (half an em at most), then the
    // other gaps between Japanese characters (a quarter em at most), then all of these evenly (3.8.4).
    auto expect2 = [&](float length, const std::string& html, const std::string& want_half, const std::string& want_full,
                       const std::string& what) {
        Laid l;
        lay_out(l, vertical, half_forms, length, html);
        const std::string got = drawn(l.c.glyphs, half_forms);
        const std::string w = half_forms ? want_half : want_full;
        check(got == w, mode + what + ": " + got + "\n     want " + w);
    };
    // JLReq Fig. 89 ②→③, 15 em lines: 、「 and 」と are 1.5 em each, and the line is half an em over; the half ems after
    // 、 and after 」 go to a quarter each and とよばれ stays on the line.
    expect(240, "仮製本には、「がんだれ」とよばれる様式がある。",
           "仮@0 製@16 本@32 に@48 は@64 、@80 「*@92 が@100 ん@116 だ@132 れ@148 」@164 と@176 よ@192 ば@208 れ@224 | "
           "る@0 様@16 式@32 が@48 あ@64 る@80 。@96",
           "3.8.3: JLReq Fig. 89 ③, 、「 and 」 squeezed to a quarter em");
    {
        Laid l;
        lay_out(l, vertical, half_forms, 240, "仮製本には、「がんだれ」とよばれる様式がある。");
        check(selected(l, vertical, 5, 12) == "80+12", mode + "selecting 、 squeezed by a quarter: " + selected(l, vertical, 5, 12));
        check(selected(l, vertical, 11, 12) == "164+12", mode + "selecting 」 squeezed by a quarter: " + selected(l, vertical, 11, 12));
        Laid even;
        lay_out(even, vertical, half_forms, 240, "仮製本には、「がんだれ」とよばれる様式がある。", "justify", line_adjustment::even);
        check(line_texts(even.c.glyphs) == "仮製本には、「がんだれ」とよば|れる様式がある。",
              mode + "line_adjustment::even pushes れ on, as before: " + line_texts(even.c.glyphs));
    }
    // JLReq Fig. 89 ④→⑤: 「前 does not fit even with 「's half em gone, so 「 starts the next line and the 14 characters
    // before it share the em (13 gaps).
    expect(240, "書籍では主体となる本文の前に「前付」がつく。",
           "書@0 籍@17.2 で@34.5 は@51.7 主@68.9 体@86.2 と@103.4 な@120.6 る@137.8 本@155.1 文@172.3 の@189.5 前@206.8 "
           "に@224 | 「*@0 前@8 付@24 」@40 が@56 つ@72 く@88 。@104",
           "3.8.2: JLReq Fig. 89 ⑤, 「 too long to squeeze in goes on");
    // The half em after 。 inside a line stays (3.8.3, note), and expanding, the gaps before and after 。 take nothing.
    expect(104, "あいう。えおかき", "あ@0 い@18.7 う@37.3 。@53.3 え@69.3 お@88 | か@0 き@16",
           "3.8.3: 。 inside a line keeps its half em; 3.1.11 ②: no space next to it");
    // 3.1.11 ②-1: a line that cannot squeeze (its 「 at the head, 」 before 。, 。 ending it) puts its 16 px between く
    // and け, its only other gap (3.8.4 d), none next to 「, 」 or 。.
    expect(72, "「くけ」。こさ", "「*@0 く@8 け@40 」*@56 。*@64 | こ@0 さ@16", "3.1.11 ②-1: no space next to brackets");
    // Middle dots inside the line before brackets: 8 px over takes both quarters of ・; 12 px over, 「's half em by 4 more.
    expect2(120, "あい・うえ「おかきく", "あ@0 い@16 ・*@32 う@40 え@56 「@80 お@88 か@104 | き@0 く@16",
            "あ@0 い@16 ・@28 う@40 え@56 「@80 お@88 か@104 | き@0 く@16", "3.8.3: a middle dot's quarters before brackets");
    expect2(116, "あい・うえ「おかきく", "あ@0 い@16 ・*@32 う@40 え@56 「@76 お@84 か@100 | き@0 く@16",
            "あ@0 い@16 ・@28 う@40 え@56 「@76 お@84 か@100 | き@0 く@16", "3.8.3: then the half em before 「");
    // A middle dot ending a line gives up both quarters when the line needs them.
    expect2(88, "あいうえお・か", "あ@0 い@16 う@32 え@48 お@64 ・*@80 | か@0", "あ@0 い@16 う@32 え@48 お@64 ・@76 | か@0",
            "3.8.3: a middle dot ending a line");
    // A paragraph's last line a little too long fits with its 。's half em (3.8.1, note).
    expect(88, "あいうえお。", "あ@0 い@16 う@32 え@48 お@64 。*@80", "3.8.1: a paragraph's last line squeezed to fit");
    // A <br> after the line's last text (Aozora-made books end every line with one): 。 before it still ends the line,
    // so its half em is still what the line squeezes.
    expect(88, "あいうえお。<br/>", "あ@0 い@16 う@32 え@48 お@64 。*@80", "3.8.1: squeezed to fit before a final <br>");
    expect(88, "あいうえお。<br/>かき", "あ@0 い@16 う@32 え@48 お@64 。*@80 | か@0 き@16", "3.8.2: squeezed to fit before a <br>");
    expect(88, "かき。<br/>\n\u00a0いうえお。<br/>\n", "か@0 き@16 。@32 | \u00a0@0 い@16 う@32 え@48 お@64 。*@80",
           "3.8.1: after a no-break space, before a final <br> and a new line");
    // An ideographic space hanging past the line's end (after 。) is not squeezed for: 」 keeps its half em.
    expect(88, "「あ」いう。　えお", "「*@0 あ@8 」@24 い@40 う@56 。@72 　@88 | え@0 お@16",
           "3.8.2: an ideographic space hanging past the end is no reason to squeeze");
    // A line of Latin text justifies as it did, evenly, even with — or … in it (the CJK rule set's Japanese script, but
    // no Japanese text on the line).
    {
        Laid jl, ev;
        lay_out(jl, vertical, half_forms, 200, "ab cd\u2014ef gh ij kl mn op qr st", "justify");
        lay_out(ev, vertical, half_forms, 200, "ab cd\u2014ef gh ij kl mn op qr st", "justify", line_adjustment::even);
        check(drawn(jl.c.glyphs, half_forms) == drawn(ev.c.glyphs, half_forms),
              mode + "a Latin line with an em dash justifies evenly: " + drawn(jl.c.glyphs, half_forms) + "\n     even " +
                  drawn(ev.c.glyphs, half_forms));
    }
    // With nothing to squeeze, a line a hair too short for its next piece breaks there, as before.
    {
        Laid jl, ev;
        lay_out(jl, vertical, half_forms, 63.995f, "あいうえお", "justify");
        lay_out(ev, vertical, half_forms, 63.995f, "あいうえお", "justify", line_adjustment::even);
        check(line_texts(jl.c.glyphs) == line_texts(ev.c.glyphs),
              mode + "nothing to squeeze, a line 0.005 px short breaks as before: " + line_texts(jl.c.glyphs) + "\n     even " +
                  line_texts(ev.c.glyphs));
    }
    // Expanding: where Japanese meets Latin (on its quarter em, 3.2.6 ④) before the gaps between kana (3.8.4 b, c), and
    // Latin word spaces first (a).
    expect(96, "かきAくけこ", "か@0 き@16 A@40 く@64 け@80 | こ@0", "3.8.4: Japanese–Latin gaps before kana ones");
    expect(128, "かきA Bくけこ", "か@0 き@16 A@38 B@74 く@96 け@112 | こ@0", "3.8.4: word spaces first");

    // JLReq 3.2.6 ④ (document::set_latin_spacing, quarter by default): a quarter em between kana or kanji and Latin
    // letters or digits, none at a line's end nor next to brackets and punctuation; line adjustment squeezes it to an
    // eighth em, last (3.8.3), and widens it after word spaces (3.8.4 b). A tate-chu-yoko number is set as a Japanese
    // character: none next to it.
    expect(400, "かきAくけ", "か@0 き@16 A@36 く@56 け@72", "3.2.6 ④: a quarter em each side of a Latin letter");
    {
        Laid none;
        lay_out(none, vertical, half_forms, 400, "かきAくけ", "justify", line_adjustment::jlreq, latin_spacing::none);
        check(drawn(none.c.glyphs, half_forms) == "か@0 き@16 A@32 く@48 け@64",
              mode + "latin_spacing::none: as before: " + drawn(none.c.glyphs, half_forms));
    }
    expect(52, "かきAく", "か@0 き@16 A@36 | く@0", "3.2.6 ④: none at a line's end");
    expect(84, "かきAくけ", "か@0 き@16 A@34 く@52 け@68", "3.8.3: squeezed to an eighth em to fit");
    expect(400, "か「A」き", "か@0 「@24 A@32 」@48 き@64", "3.2.6 ④: none next to brackets");
    if (!vertical) expect(400, "第12回", "第@0 1@20 2@36 回@56", "3.2.6 ④: a number's quarter ems");
    // A table measures its columns with the quarter em: the cell 注A is not squeezed back into a column measured
    // without it (jlreq-in-japanese's footnote tables, 注1）; a letter here, as a lone digit is tate-chu-yoko in
    // vertical text).
    {
        Laid l;
        lay_out_body(l, vertical, half_forms, 400,
                     "<table style=\"border-spacing:0\"><tr><td style=\"padding:0;white-space:nowrap\">注A</td>"
                     "<td style=\"padding:0\">あ</td></tr></table>");
        check(drawn(l.c.glyphs, half_forms) == "注@0 A@20 あ@36",
              mode + "3.2.6 ④: a table's column measured with the quarter em: " + drawn(l.c.glyphs, half_forms));
    }

    // JLReq 3.1.1 ②: in horizontal text ，． are 読点 and 句点 (cl-07, cl-06) as 、。 are: the half em after one ending a
    // wrapped line goes, 「 after one halves, a closing bracket before one halves, line adjustment squeezes ， (not ．
    // inside a line) and puts no space next to them, no quarter em next to Latin. Each case is laid out with ，． and
    // must draw what it draws with 、。. In vertical text, and in text marked as Chinese (where ， may be centred),
    // they stay as they were (comma_period::none).
    {
        auto with = [&](float length, const std::string& html, comma_period mode_cp, const char* lang) {
            Laid l;
            l.c.half_forms = half_forms;
            if (vertical) l.c.h = length;
            l.doc = document::createFromString(std::string("<html") + (vertical ? " style=\"writing-mode:vertical-rl\"" : "") +
                                                   (lang ? std::string(" lang=\"") + lang + "\"" : std::string()) +
                                                   "><body style=\"margin:0;font-size:16px;line-height:20px\"><p style=\"margin:0;text-align:justify\">" +
                                                   html + "</p></body></html>",
                                               &l.c);
            l.doc->set_comma_period(mode_cp);
            l.doc->render(pixel_t(vertical ? 400 : length));
            l.doc->draw(0, 0, 0, nullptr);
            return drawn(l.c.glyphs, true);
        };
        auto swap = [](std::string s, bool to_fullwidth) {
            const std::pair<std::string, std::string> marks[] = {{"、", "，"}, {"。", "．"}};
            for (const auto& [ja, fw] : marks) {
                const std::string& from = to_fullwidth ? ja : fw;
                const std::string& to = to_fullwidth ? fw : ja;
                for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
            }
            return s;
        };
        const std::pair<float, std::string> cases[] = {
            {96, "あいうえお。かきく"},  {96, "あいうえお、かきく"},   {400, "詰め寄った。「主人"},
            {400, "あ、「い"},           {104, "あいう」。えお"},      {400, "あ。」い"},
            {104, "あいう。えおかき"},   {88, "あいうえお。"},         {400, "あ、Aい"},
            {240, "仮製本には、「がんだれ」とよばれる様式がある。"},
        };
        for (const auto& [length, html] : cases) {
            const std::string fw = swap(html, true);
            const std::string got = with(length, fw, comma_period::jlreq, nullptr);
            if (!vertical) {
                const std::string want = swap(with(length, html, comma_period::jlreq, nullptr), true);
                check(got == want, mode + "3.1.1 ②: " + fw + " as " + html + ": " + got + "\n     want " + want);
            } else {
                const std::string want = with(length, fw, comma_period::none, nullptr);
                check(got == want, mode + "3.1.1 ②: vertical " + fw + " as before: " + got + "\n     want " + want);
            }
            const std::string zh = with(length, fw, comma_period::jlreq, "zh-CN");
            const std::string zh_before = with(length, fw, comma_period::none, "zh-CN");
            check(zh == zh_before, mode + "3.1.1 ②: " + fw + " in Chinese text as before: " + zh + "\n     want " + zh_before);
        }
    }
    // JLReq 3.1.3: 、 between kanji numerals (二、三日) and ・ as their decimal point (三・五, 零・五) are set solid.
    expect(400, "二、三日", "二@0 、*@16 三@24 日@40", "3.1.3: 、 between kanji numerals, solid");
    expect2(400, "三・五倍", "三@0 ・*@16 五@24 倍@40", "三@0 ・@12 五@24 倍@40", "3.1.3: ・ between kanji numerals, solid");
    expect2(400, "零・五倍", "零@0 ・*@16 五@24 倍@40", "零@0 ・@12 五@24 倍@40", "3.1.3: ・ after 零, solid");
    expect(400, "零、五", "零@0 、*@16 五@24", "3.1.3: 、 after 零, solid");
    expect(400, "二、さん", "二@0 、@16 さ@32 ん@48", "3.1.3: not before kana");
    // JLReq 3.1.6 (hyphens): ‐ (U+2010) is 0.25em and ゠ (U+30A0) is 0.50em when full-width.
    expect(400, "あ\u2010い", "あ@0 \u2010@10 い@20", "3.1.6 (hyphens): ‐ (U+2010) trimmed to 0.25em (4px centered in 16px)");
    expect2(400, "あ\u30A0い", "あ@0 \u30A0*@16 い@24", "あ@0 \u30A0@12 い@24",
            "3.1.6 (hyphens): ゠ (U+30A0) trimmed to 0.50em (8px centered in 16px)");
    // JLReq 3.1.6 ① (document::set_dividing_space, full_em by default): a full-width space after ？！ ending a sentence
    // when the book has none; 0.5em before full-width opening bracket (total 1.0em visual gap with 「's lead half-em);
    // full-width before Latin/digits and normal sentence-starting kana (no single-kana particle blacklist); none before
    // closing brackets, not-starting marks (っ, ！, ？, 。, 、), spaces (U+3000), nor at a line's end.
    expect(400, "なんだ！そうか", "な@0 ん@16 だ@32 ！@48 そ@80 う@96 か@112", "3.1.6 ①: a full-width space after ！");
    expect(400, "なんだ！もう帰る", "な@0 ん@16 だ@32 ！@48 も@80 う@96 帰@112 る@128", "3.1.6 ①: full-width space before も");
    expect(400, "そうだ！なんだ", "そ@0 う@16 だ@32 ！@48 な@80 ん@96 だ@112", "3.1.6 ①: full-width space before な");
    expect(400, "そうか？はい", "そ@0 う@16 か@32 ？@48 は@80 い@96", "3.1.6 ①: full-width space before は");
    expect(400, "えっ！かまわない", "え@0 っ@16 ！@32 か@64 ま@80 わ@96 な@112 い@128", "3.1.6 ①: full-width space before か");
    expect(400, "本当？わたしは", "本@0 当@16 ？@32 わ@64 た@80 し@96 は@112", "3.1.6 ①: full-width space before わ");
    expect(400, "なんだ！よし", "な@0 ん@16 だ@32 ！@48 よ@80 し@96", "3.1.6 ①: full-width space before よ");
    expect(400, "なんだ！100円", "な@0 ん@16 だ@32 ！@48 1@80 0@96 0@112 円@132", "3.1.6 ①: full-width space before ASCII digits");
    expect(400, "本当？OK", "本@0 当@16 ？@32 O@64 K@80", "3.1.6 ①: full-width space before Latin");
    expect(400, "なんだ！「そうか」", "な@0 ん@16 だ@32 ！@48 「@80 そ@88 う@104 か@120 」@136",
           "3.1.6 ①: 1.0em total visual gap before 「 (0.5em + 「's leading 0.5em)");
    expect(400, "何？って", "何@0 ？@16 っ@32 て@48", "3.1.6 ①: none before small っ (not_starting)");
    expect(400, "なんだ！？そうか", "な@0 ん@16 だ@32 ！@48 ？@64 そ@96 う@112 か@128", "3.1.6 ①: none between ！ and ？, 1em after ？");
    expect(400, "えっ！」と", "え@0 っ@16 ！@32 」@48 と@64", "3.1.6 ①: none before 」");
    expect(400, "あ！　い", "あ@0 ！@16 　@32 い@48", "3.1.6 ①: none where the book has U+3000");
    expect(64, "あいう！そこ", "あ@0 い@16 う@32 ！@48 | そ@0 こ@16", "3.1.6 ①: none at a line's end");
    {
        Laid none;
        lay_out(none, vertical, half_forms, 400, "なんだ！そうか", "justify", line_adjustment::jlreq, latin_spacing::quarter,
                dividing_space::none);
        check(drawn(none.c.glyphs, half_forms) == "な@0 ん@16 だ@32 ！@48 そ@64 う@80 か@96",
              mode + "dividing_space::none: as before: " + drawn(none.c.glyphs, half_forms));
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
