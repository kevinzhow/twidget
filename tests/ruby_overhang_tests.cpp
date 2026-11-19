// litehtml without TwidgetCore: ruby longer than its base overhangs the kana next to it (JLReq 3.3.8), horizontal and
// vertical-rl.
//
// JLReq 3.3.8: a ruby longer than its base may overhang the characters next to the base by up to one ruby character
// (rule 2: hiragana, katakana, the prolonged sound mark, small kana), never a kanji (rule 1) nor another ruby (rule 2,
// note); where it may not, the base is spaced (3.3.5: the ruby centred on its base). Next to punctuation
// (document::set_ruby_overhang, punctuation by default): into the half em after 」、。 before it or before 「 after it
// (rule 3, no further than that space), onto …… and ―― (rule 4) and middle dots (rule 5), and onto 「 before it or
// 」、。 after it, no further than that character (rule 6); each up to one ruby character. A space a ruby overhangs is
// no longer there for line adjustment to squeeze (rule 3, note). At a line's head the base and its ruby start together
// and at a line's end they end together (line-edge a, b); a ruby never reaches past the line.
//
// The stub measures a character as wide as its font's size (16 px text, 8 px ruby: rt is font-size 50%), breaks a line
// between any two characters the CJK rule set does not keep together, and records what it draws along the line: each
// base text "text@x", each annotation "^text@x" (an annotation's pieces drawn one after another are one).
#include <litehtml.h>
#include <litehtml/cjk_rules.h>

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

struct Drawn {
    std::string text;
    float x, y, width;
    bool ruby;
};

struct Stub : document_container {
    float w = 400, h = 400;
    std::vector<Drawn> drawn;
    // The font is its size.
    uint_ptr create_font(const font_description& d, const document*, font_metrics* fm) override {
        if (fm) {
            fm->font_size = d.size;
            fm->ascent = d.size * 0.8f;
            fm->descent = d.size * 0.2f;
            fm->height = d.size;
            fm->x_height = d.size * 0.5f;
            fm->ch_width = d.size / 2;
            fm->draw_spaces = false;
        }
        return uint_ptr(std::lround(float(d.size)));
    }
    void delete_font(uint_ptr) override {}
    pixel_t text_width(const char* text, uint_ptr font) override {
        return pixel_t(float(font) * float(code_points(text).size()));
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
            } else if (next && !cjk::no_break_between(cp, next)) {
                on_word(s.substr(start, end - start).c_str());
                start = end;
            }
        }
        if (s.size() > start) on_word(s.substr(start).c_str());
    }
    void draw_text(uint_ptr, const char* text, uint_ptr font, web_color, const position& pos) override {
        const bool ruby = font < 16;
        const float width = float(font) * float(code_points(text).size());
        if (ruby && !drawn.empty() && drawn.back().ruby && std::abs(drawn.back().y - float(pos.y)) < 0.5f &&
            std::abs(drawn.back().x + drawn.back().width - float(pos.x)) < 0.5f) {
            drawn.back().text += text;
            drawn.back().width += width;
            return;
        }
        drawn.push_back({text, float(pos.x), float(pos.y), width, ruby});
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

// What was drawn, in order: base texts "t@x", annotations "^t@x"; a base text on another line than the base text
// before it starts with " | ".
std::string format(const std::vector<Drawn>& drawn) {
    std::string out;
    bool any = false;
    float y = 0;
    for (const auto& d : drawn) {
        if (!d.ruby) {
            if (any && std::abs(d.y - y) >= 1) out += " |";
            any = true;
            y = d.y;
        }
        out += (out.empty() ? "" : " ") + std::string(d.ruby ? "^" : "") + d.text + "@" + num(d.x);
    }
    return out;
}

// <p>html</p>, left-aligned, in a line (column) `length` px long.
std::string lay_out(bool vertical, float length, const std::string& html, ruby_overhang overhang = ruby_overhang::punctuation) {
    Stub c;
    if (vertical) c.h = length;
    auto doc = document::createFromString(std::string("<html") + (vertical ? " style=\"writing-mode:vertical-rl\"" : "") +
                                              "><body style=\"margin:0;font-size:16px;line-height:32px\"><p style=\"margin:0\">" +
                                              html + "</p></body></html>",
                                          &c);
    doc->set_ruby_overhang(overhang);
    doc->render(pixel_t(vertical ? 400 : length));
    doc->draw(0, 0, 0, nullptr);
    return format(c.drawn);
}

std::string r(const std::string& base, const std::string& ruby) { return "<ruby>" + base + "<rt>" + ruby + "</rt></ruby>"; }

void run(bool vertical) {
    const std::string mode = vertical ? "vertical: " : "horizontal: ";
    // `horizontal`: what horizontal text draws when it differs (a word whose characters are not trimmed is drawn whole:
    // あ」@0, where vertical text draws あ@0 」@16).
    auto expect = [&](float length, const std::string& html, const std::string& want, const std::string& what,
                      const std::string& horizontal = "") {
        const std::string got = lay_out(vertical, length, html);
        const std::string w = !vertical && !horizontal.empty() ? horizontal : want;
        check(got == w, mode + what + ": " + got + "\n     want " + w);
    };
    const std::string shun = r("旬", "しゅん");

    // JLReq's figure (3.3.8): しゅん over 旬 is half an em longer than 旬, a quarter em on each side.
    expect(400, "の" + shun + "に", "の@0 ^しゅん@12 旬@16 に@32", "rule 2: の旬に, the ruby overhangs both kana, 旬 set solid");
    expect(400, "後" + shun + "又", "後@0 ^しゅん@16 旬@20 又@40", "rule 1: 後旬又, no overhang onto kanji, 旬 spaced (as before)");
    expect(400, "の" + shun + "又", "の@0 ^しゅん@12 旬@16 又@36", "の旬又: onto の only, space before 又");
    expect(400, "後" + shun + "に", "後@0 ^しゅん@16 旬@20 に@36", "後旬に: onto に only, space after 後");
    expect(400, "ァ" + shun + "ー", "ァ@0 ^しゅん@12 旬@16 ー@32", "rule 2: small kana and ー are overhung too");
    expect(400, "の" + r("旬", "しゅんか") + "に", "の@0 ^しゅんか@8 旬@16 に@32", "rule 2: up to one ruby character a side");
    expect(400, "の" + r("旬", "しゅんしゅん") + "に", "の@0 ^しゅんしゅん@8 旬@24 に@48",
           "rule 2: no more than one ruby character a side, the rest spaces the base");
    expect(400, "あ" + shun + shun + "い", "あ@0 ^しゅん@12 旬@16 ^しゅん@36 旬@40 い@56",
           "rule 2 note: a ruby does not overhang the ruby next to it");
    expect(400, "あ「" + shun + "」", "あ@0 「@16 ^しゅん@28 旬@32 」@48", "rule 6: onto 「 before and 」 after, 旬 set solid");
    {
        const std::string got = lay_out(vertical, 400, "あ「" + shun + "」", ruby_overhang::kana);
        check(got == "あ@0 「@16 ^しゅん@32 旬@36 」@56", mode + "ruby_overhang::kana: no overhang next to brackets, as before: " + got);
    }
    expect(400, "の" + r("旬", "しゅん") + "あ", "の@0 ^しゅん@12 旬@16 あ@32", "a following あ");
    // Rules 3 to 6 (JLReq Fig. 135, 136): the half em after 」、。 before the ruby and before 「 after it; ……, ・; 」、。
    // after it and 「 before it, onto the mark itself.
    expect(400, "あ」" + shun + "に", "あ@0 」@16 ^しゅん@28 旬@32 に@48", "rule 3: into the half em after 」",
           "あ」@0 ^しゅん@28 旬@32 に@48");
    expect(400, "あ、" + shun + "に", "あ@0 、@16 ^しゅん@28 旬@32 に@48", "rule 3: into the half em after 、",
           "あ、@0 ^しゅん@28 旬@32 に@48");
    expect(400, "あ。" + shun + "に", "あ@0 。@16 ^しゅん@28 旬@32 に@48", "rule 3: into the half em after 。",
           "あ。@0 ^しゅん@28 旬@32 に@48");
    expect(400, "の" + shun + "「あ", "の@0 ^しゅん@12 旬@16 「@32 あ@48", "rule 3: into the half em before 「",
           "の@0 ^しゅん@12 旬@16 「あ@32");
    expect(400, "の" + shun + "、あ", "の@0 ^しゅん@12 旬@16 、@32 あ@48", "rule 6: onto 、 after it");
    expect(400, "あ……" + shun + "……い", "あ@0 …@16 …@32 ^しゅん@44 旬@48 …@64 …@80 い@96", "rule 4: onto ……",
           "あ@0 ……@16 ^しゅん@44 旬@48 ……@64 い@96");
    expect(400, "あ・" + shun + "・い", "あ@0 ・@16 ^しゅん@28 旬@32 ・@48 い@64", "rule 5: onto middle dots",
           "あ・@0 ^しゅん@28 旬@32 ・@48 い@64");
    // No further than one ruby character, the half em of rule 3, nor the mark itself (rule 6): the rest spaces the base.
    const std::string shunshun = r("旬", "しゅんしゅん");
    expect(400, "あ」" + shunshun + "「い", "あ@0 」@16 ^しゅんしゅん@24 旬@40 「@64 い@80", "rule 3: no further than the half em",
           "あ」@0 ^しゅんしゅん@24 旬@40 「い@64");
    expect(400, "あ「" + shunshun + "」", "あ@0 「@16 ^しゅんしゅん@24 旬@40 」@64", "rule 6: no further than the mark itself");
    // Group and jukugo ruby: by the base's outer gaps, the first pair's before and the last pair's after.
    expect(400, "あ「" + r("東京", "とうきょうと") + "」", "あ@0 「@16 ^とうきょうと@28 東@32 京@56 」@72", "rules 3 to 6: a group ruby");
    expect(400, "あ「<ruby>旬<rt>しゅん</rt>旬<rt>しゅん</rt></ruby>」", "あ@0 「@16 ^しゅん@28 旬@32 ^しゅん@52 旬@56 」@72",
           "rules 3 to 6: a ruby of two pairs (jukugo)");
    // Line edges: at a line's head the excess goes after the base, into 「's half em; at a wrapped line's end the
    // whole excess before it, into 、's.
    expect(400, shun + "「あ", "^しゅん@0 旬@0 「@16 あ@32", "line-edge a: at the line's head, into the half em before 「",
           "^しゅん@0 旬@0 「あ@16");
    expect(80, "あいう、" + shun + "えお", "あ@0 い@16 う@32 、@48 ^しゅん@56 旬@64 | え@0 お@16",
           "line-edge b: at a wrapped line's end, into the half em after 、", "あ@0 い@16 う、@32 ^しゅん@56 旬@64 | え@0 お@16");
    // Rule 3, note: what a ruby overhangs of 」's half em is not squeezed for the line (JLReq 3.8.3) as well.
    expect(124, "あ」" + shun + "にいうえお", "あ@0 」@16 ^しゅん@24 旬@28 に@44 い@60 う@76 え@92 お@108",
           "rule 3: line adjustment squeezes only what the ruby leaves of the half em");
    expect(120, "あ」" + shun + "にいうえお", "あ@0 」@16 ^しゅん@28 旬@32 に@48 い@64 う@80 え@96 | お@0",
           "rule 3: not what the ruby overhangs", "あ」@0 ^しゅん@28 旬@32 に@48 い@64 う@80 え@96 | お@0");
    expect(400, "の" + r("漢字", "かんじい") + "に", "の@0 ^かんじい@16 漢@16 字@32 に@48", "a ruby no longer than its base: as before");
    expect(400, "あ" + r("東京", "とうきょうと") + "い", "あ@0 ^とうきょうと@12 東@16 京@40 い@56",
           "a group ruby longer than its base overhangs by its base's outer gaps");
    expect(400, "の<ruby>旬<rt>しゅん</rt>旬<rt>しゅん</rt></ruby>に", "の@0 ^しゅん@12 旬@16 ^しゅん@36 旬@40 に@56",
           "a ruby of two pairs overhangs by its first pair before and its last pair after");
    // The overhang counts when the line breaks: の旬にあ fits in four ems.
    expect(64, "の" + shun + "にあ", "の@0 ^しゅん@12 旬@16 に@32 あ@48", "the line holds what the overhang makes room for");
    // Line edges (a, b): at a line's head the base and its ruby start together; at a wrapped line's end they end
    // together. The ruby never reaches past the line.
    expect(400, shun + "のあ", "^しゅん@0 旬@0 の@16 あ@32", "line-edge a: at the line's head, 旬 and しゅん start together, onto の");
    expect(400, shun + "又", "^しゅん@0 旬@0 又@24", "line-edge a: at the line's head before a kanji, the space after 旬");
    expect(72, "あいう" + shun + "えお", "あ@0 い@16 う@32 ^しゅん@40 旬@48 | え@0 お@16",
           "line-edge b: at a wrapped line's end, 旬 and しゅん end together, onto う");
    expect(80, "あいうえ" + shun + "おか", "あ@0 い@16 う@32 え@48 ^しゅん@56 旬@64 | お@0 か@16",
           "line-edge b: a ruby that fits only with its whole excess over the kana before it ends the line there");
    expect(80, "あいう漢" + shun + "おか", "あ@0 い@16 う@32 漢@48 ^しゅん@0 | 旬@0 お@16 か@32",
           "line-edge b: after a kanji it does not fit, and starts the next line");
    // JLReq's rules are for Japanese: a ruby over Latin text (IDPF hefty-water's 2H₂ under "hydrogen") stays as
    // CSS lays it out, its base centred, also at a line's head and next to kana.
    expect(400, r("ab", "abcdefgh") + "の", "^abcdefgh@0 a@8 b@40 の@64", "a ruby over Latin text: no line-edge alignment, no overhang");
    expect(400, "の" + r("ab", "abcdefgh") + "の", "の@0 ^abcdefgh@16 a@24 b@56 の@80", "a ruby over Latin text after a kana: no overhang");
    expect(72, "あい漢" + shun + "えお", "あ@0 い@16 漢@32 ^しゅん@48 旬@56 | え@0 お@16",
           "line-edge b: at a wrapped line's end after a kanji, the space before 旬");
}

} // namespace

int main() {
    for (bool vertical : {false, true}) run(vertical);
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
