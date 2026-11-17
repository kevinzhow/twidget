// litehtml without TwidgetCore: tate-chu-yoko (縦中横) in vertical-rl text. As stone_engine sets it, a token of one or two
// half-width digits stands upright, side by side in one em of the column (「3年1月5日」, 「第12回」); three digits, or digits
// next to a full-width one (「1２」), stay sideways. Ours adds !? !! ?! ?? standing alone between Japanese characters
// (Kevin's 「んですか!?」」), and ‼ ⁉ ⁈ ⁇, which are upright already, taking one em. text-combine-upright: digits 2 (or
// digits) does the same; all sets the whole text in one em. Wider than the em, the token is squeezed across it. Where
// each token is drawn, where the column breaks, where justify puts its space, and where selection highlights it;
// horizontal text is unchanged.
//
// The stub measures by character, horizontally: a digit 8.8 px (0.55 em, so two are wider than the 16 px em), ! and ? 5,
// a Latin letter 8, ‼ ⁉ ⁈ ⁇ 13.6, anything else 16. It breaks a line between any two characters the CJK rule set does
// not keep together, but not between two ASCII characters (as UAX #14 keeps a number or a word together).
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

float width_of(char32_t c) {
    if (c >= '0' && c <= '9') return 8.8f;
    if (c == '!' || c == '?') return 5;
    if (c < 0x80) return 8;
    if (c == U'‼' || c == U'⁉' || c == U'⁈' || c == U'⁇') return 13.6f;
    return 16;
}

// A text drawn, in laid-out coordinates: where it starts along the line and how long it is there, how wide it is across
// the line (an upright text: its horizontal width, squeezed), the line's middle across it, and whether it stands upright.
struct Drawn {
    std::string text;
    float start, length, across, mid;
    bool upright;
};

struct Stub : document_container {
    float w = 400, h = 400;
    std::vector<transform_matrix> groups;
    std::vector<Drawn> drawn;
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
    pixel_t text_width(const char* text, uint_ptr) override {
        float w = 0;
        for (auto [cp, at] : code_points(text)) w += width_of(cp);
        return pixel_t(w);
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
            } else if (next && !(cp < 0x80 && next < 0x80 && next != ' ') && !cjk::no_break_between(cp, next)) {
                on_word(s.substr(start, end - start).c_str());
                start = end;
            }
        }
        if (s.size() > start) on_word(s.substr(start).c_str());
    }
    void begin_group(uint_ptr, const paint_group& group) override { groups.push_back(group.matrix); }
    void end_group(uint_ptr) override { groups.pop_back(); }
    // An upright text is drawn horizontally in a group that turns it back: its box, mapped by the group's matrix, is
    // where it stands in the line.
    void draw_text(uint_ptr, const char* text, uint_ptr, web_color, const position& pos) override {
        const float tw = float(text_width(text, 0));
        if (!groups.empty() && groups.back().a == 0 && groups.back().b < 0) {
            const transform_matrix& m = groups.back();
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            for (float x : {float(pos.x), float(pos.x) + tw})
                for (float y : {float(pos.y), float(pos.y) + float(pos.height)}) {
                    const float lx = m.a * x + m.c * y + m.tx, ly = m.b * x + m.d * y + m.ty;
                    x0 = std::min(x0, lx);
                    x1 = std::max(x1, lx);
                    y0 = std::min(y0, ly);
                    y1 = std::max(y1, ly);
                }
            drawn.push_back({text, x0, x1 - x0, y1 - y0, (y0 + y1) / 2, true});
        } else {
            drawn.push_back({text, float(pos.x), tw, float(pos.height), float(pos.y) + float(pos.height) / 2, false});
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

// Each line's texts as "text@start" (sideways) or "text^across@start" (upright, `across` wide); lines split by " | ".
std::string listed(const std::vector<Drawn>& drawn) {
    std::string out;
    float mid = drawn.empty() ? 0 : drawn.front().mid;
    for (const auto& d : drawn) {
        if (std::abs(d.mid - mid) >= 1) out += " |";
        mid = d.mid;
        out += (out.empty() ? "" : " ") + d.text + (d.upright ? "^" + num(d.across) : "") + "@" + num(d.start);
    }
    return out;
}

struct Laid {
    Stub c;
    std::shared_ptr<document> doc;
};

// <p>html</p> in a line (column) `length` px long.
void lay_out(Laid& l, bool vertical, float length, const std::string& html, const char* align = "left") {
    if (vertical) l.c.h = length;
    l.doc = document::createFromString(std::string("<html") + (vertical ? " style=\"writing-mode:vertical-rl\"" : "") +
                                           "><body style=\"margin:0;font-size:16px;line-height:20px\"><p style=\"margin:0;text-align:" +
                                           align + "\">" + html + "</p></body></html>",
                                       &l.c);
    l.doc->render(pixel_t(vertical ? 400 : length));
    l.doc->draw(0, 0, 0, nullptr);
}

// Where selecting the `n`th drawn text (from 2 px into it to 2 px before its end) highlights along the line.
std::string selected(Laid& l, bool vertical, size_t n) {
    if (n >= l.c.drawn.size()) return "none";
    const Drawn& d = l.c.drawn[n];
    text_selection ts(*l.doc);
    position view;
    l.c.get_viewport(view);
    auto page = [&](float x, float y, float& px, float& py) {
        px = vertical ? float(view.width) - y : x;
        py = vertical ? x : y;
    };
    float x0, y0, x1, y1;
    page(d.start + 2, d.mid, x0, y0);
    page(d.start + d.length - 2, d.mid, x1, y1);
    const auto rects = ts.rects(ts.position_at(x0, y0), ts.position_at(x1, y1));
    if (rects.size() != 1) return "rects " + std::to_string(rects.size());
    return num(float(vertical ? rects[0].y : rects[0].x)) + "+" + num(float(vertical ? rects[0].height : rects[0].width));
}

// The tokens cjk::tate_chu_yoko finds in text[from, to), "|"-separated.
std::string tokens(const std::string& text, size_t from = 0, size_t to = std::string::npos) {
    std::string out;
    for (const auto& t : cjk::tate_chu_yoko(text, from, to == std::string::npos ? text.size() : to))
        out += (out.empty() ? "" : "|") + text.substr(t.from, t.to - t.from);
    return out;
}

void expect(bool vertical, float length, const std::string& html, const std::string& want, const std::string& what,
            const char* align = "left") {
    Laid l;
    lay_out(l, vertical, length, html, align);
    const std::string got = listed(l.c.drawn);
    check(got == want, std::string(vertical ? "vertical: " : "horizontal: ") + what + ": " + got + "\n     want " + want);
}

} // namespace

int main() {
    // stone's rule: a token of one or two half-width digits, upright in one em; two digits (17.6 px) squeezed to it.
    expect(true, 400, "3年1月5日", "3^8.8@0 年^16@16 1^8.8@32 月^16@48 5^8.8@64 日^16@80", "single digits upright, an em each");
    expect(true, 400, "第12回", "第^16@0 12^16@16 回^16@32", "two digits side by side in one em");
    expect(true, 400, "第123回", "第^16@0 123@16 回^16@42.4", "three digits stay sideways");
    expect(true, 400, "第1２回", "第^16@0 1@16 ２^16@24.8 回^16@40.8", "a half-width digit next to a full-width one is not combined");
    expect(true, 400, "全12.5倍", "全^16@0 12.5@16 倍^16@50.4", "a decimal number is one token, sideways");
    expect(true, 400, "あA12い", "あ^16@0 A12@16 い^16@41.6", "digits after a letter are one token with it, sideways");
    // text-combine-upright: digits 2 and digits do the same; all sets the whole text in one em.
    expect(true, 400, "第<span style=\"text-combine-upright:digits 2\">12</span>回", "第^16@0 12^16@16 回^16@32",
           "text-combine-upright: digits 2");
    expect(true, 400, "第<span style=\"text-combine-upright:digits\">12</span>回", "第^16@0 12^16@16 回^16@32",
           "text-combine-upright: digits");
    expect(true, 400, "第<span style=\"text-combine-upright:all\">123</span>回", "第^16@0 123^16@16 回^16@32",
           "text-combine-upright: all");
    // Ours: !? !! ?! ?? between Japanese characters, upright in one em (Kevin's page), and ‼ ⁉ ⁈ ⁇ an em each.
    expect(true, 400, "んですか!?」", "ん^16@0 で^16@16 す^16@32 か^16@48 !?^10@64 」^16@80", "Kevin's 「んですか!?」」");
    expect(true, 400, "え!!あ??う?!", "え^16@0 !!^10@16 あ^16@32 ??^10@48 う^16@64 ?!^10@80", "!! ?? ?! too");
    expect(true, 400, "か⁉か‼か⁈か⁇", "か^16@0 ⁉^13.6@16 か^16@32 ‼^13.6@48 か^16@64 ⁈^13.6@80 か^16@96 ⁇^13.6@112",
           "‼ ⁉ ⁈ ⁇ take an em");
    expect(true, 400, "あReally!?あ", "あ^16@0 Really!?@16 あ^16@74", "!? ending a Latin word stays sideways");
    expect(true, 400, "あ!?!あ", "あ^16@0 !?!@16 あ^16@31", "!?! stays sideways");
    // The column fits a token by its em, and never splits it.
    expect(true, 64, "あいう12え", "あ^16@0 い^16@16 う^16@32 12^16@48 | え^16@0", "a column fits two digits in one em");
    expect(true, 56, "あいう12え", "あ^16@0 い^16@16 う^16@32 | 12^16@0 え^16@16", "two digits go to the next column together");
    // Justify: the line's free space goes between characters, none inside a token (5 gaps, 0.8 px each).
    expect(true, 100, "あ12いうえおかきくけこ", "あ^16@0 12^16@16.8 い^16@33.6 う^16@50.4 え^16@67.2 お^16@84 | か^16@0 き^16@16 く^16@32 け^16@48 こ^16@64",
           "justify puts no space inside a token", "justify");
    // Selecting a token highlights its em.
    {
        Laid l;
        lay_out(l, true, 400, "第12回");
        check(selected(l, true, 1) == "16+16", "vertical: selecting 12 in 第12回: " + selected(l, true, 1));
        check(selected(l, true, 2) == "32+16", "vertical: selecting 回 after it: " + selected(l, true, 2));
    }
    {
        Laid l;
        lay_out(l, true, 400, "んですか!?」");
        // か!?」 is one piece, drawn as three texts: か, !?, 」.
        check(selected(l, true, 4) == "64+16", "vertical: selecting !? in か!?」: " + selected(l, true, 4));
        check(selected(l, true, 5) == "80+16", "vertical: selecting 」 after it: " + selected(l, true, 5));
    }
    // The rule (cjk::tate_chu_yoko): a token ends at a word boundary, as stone's Intl.Segmenter("ja") draws it.
    auto rule = [](const std::string& text, const std::string& want, const std::string& what, size_t from = 0,
                   size_t to = std::string::npos) {
        const std::string got = tokens(text, from, to);
        check(got == want, "the rule: " + what + ": \"" + got + "\", want \"" + want + "\"");
    };
    rule("12時30分、第3回", "12|30|3", "numbers between kanji");
    rule("12,000円 12:30 12/25 1_2 3’", "12|30|12|25|3", "12,000 and 1_2 are one token each; : / ’ end one");
    rule("Ⅱ2 x2 ２1 1２ ²", "", "a letter, a Roman numeral or a full-width digit next to it; ² is not an ASCII digit");
    rule("か!?」 か!!? Wow!? ‼⁉", "!?|‼|⁉", "the overlay's tokens, not next to Latin");
    // A piece of a longer text: the text around it decides where a token ends; one running past the piece is not
    // returned.
    rule("あ1２あ", "", "1 before ２, as a piece", 3, 4);
    rule("あ12あ", "", "half of 12", 3, 4);
    rule("あ12あ", "12", "12 as a piece", 3, 5);

    // Horizontal text is unchanged.
    expect(false, 400, "第12回", "第@0 12@16 回@33.6", "digits");
    expect(false, 400, "か!?」", "か!?」@0", "!?");
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
