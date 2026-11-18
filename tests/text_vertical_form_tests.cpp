// twidget::text, vertical text: the vertical form of … ‥ (OpenType vert / vrt2) comes from the Japanese font of the
// list when the face that has the character first has none. Kevin's phone draws a Japanese book in `serif`, which is
// Noto Serif (Latin, a … with no vertical form) before Noto Serif CJK: the … went sideways, its dots on the Latin
// baseline, off the column's middle. The Japanese font's vertical form is a column of dots centred in the em.
// When no face of the list has a vertical form, nothing changes (the character stays sideways, as Blink draws it).
// Fixtures: tests/fixtures/fonts (Pacifico.woff: a Latin font with …, no vert; NotoSerifCJKjp-subset.otf: 一 あ … ‥
// with their vert/vrt2 forms; NotoSerifCJKjp-subset-novert.otf: the same without them; SIL Open Font License, the
// files beside them).
#include "text_backend.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <hb.h>

static int g_failed = 0;
#define CHECK(cond, ...)                                               \
    do {                                                               \
        if (!(cond)) {                                                 \
            g_failed++;                                                \
            std::printf("FAIL %s:%d: %s ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                                  \
            std::printf("\n");                                         \
        }                                                              \
    } while (0)

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int main() {
    using namespace twtext;
    TextBackend backend;
    twlite::FontMetrics m{};
    CHECK(backend.add_book_font("Latin", 400, false, read_file(TWT_FIXTURES "/Pacifico.woff")), "Latin loads");
    CHECK(backend.add_book_font("Japanese", 400, false, read_file(TWT_FIXTURES "/NotoSerifCJKjp-subset.otf")), "Japanese loads");

    const std::string ellipsis = "\xE2\x80\xA6";
    const std::string list = "\"Latin\", \"Japanese\", serif";
    const int font = backend.create_font(list, 16, 400, false, "", m);
    const int upright = backend.create_font(list, 16, 400, false, "vert=1", m);
    const auto& faces = backend.fonts().at(font).bookFaces;
    CHECK(faces.size() == 2, "both book faces in the list: %zu", faces.size());
    if (faces.size() != 2) return 1;
    Face* latin = faces[0];
    Face* japanese = faces[1];

    // The Japanese face's own glyphs for … ‥ and their vertical forms.
    auto glyph = [&](Face* face, uint32_t cp) {
        hb_codepoint_t g = 0;
        hb_font_get_nominal_glyph(face_hb_font(face), cp, &g);
        return g;
    };

    // (Pacifico has no ‥: it already falls back to the Japanese face, vertical form and all.)
    for (const auto& [text, cp, name] : {std::tuple{ellipsis, 0x2026u, "…"}}) {
        // Kevin's case: Latin first. The character has a vertical form (the Japanese font's), and the upright font draws
        // it with that form: one glyph of the Japanese face, not its horizontal glyph, an em down the column.
        CHECK(backend.has_vertical_form(font, text), "%s: a vertical form from the Japanese font", name);
        const auto runs = backend.shape(upright, text);
        CHECK(runs.size() == 1 && runs[0].face == japanese, "%s: drawn upright in the Japanese face", name);
        if (runs.size() == 1 && runs[0].face == japanese)
            CHECK(runs[0].glyphs.size() == 1 && runs[0].glyphs[0] != glyph(japanese, cp), "%s: its vertical form (vert)", name);
        CHECK(std::fabs(backend.text_width(upright, text) - 16) < 0.01f, "%s: an em along the column: %g", name,
              backend.text_width(upright, text));
        // The font without vert (sideways runs, horizontal text) keeps the Latin face's glyph.
        const auto plain = backend.shape(font, text);
        CHECK(plain.size() == 1 && plain[0].face == latin, "%s: without vert, still the Latin face", name);
    }

    // ……: two of them, each its vertical form.
    {
        const auto runs = backend.shape(upright, ellipsis + ellipsis);
        CHECK(runs.size() == 1 && runs[0].face == japanese && runs[0].glyphs.size() == 2, "……: two vertical forms");
    }

    // Kana and kanji keep their face, and Latin letters in the upright font stay in the Latin face.
    {
        const auto runs = backend.shape(upright, "A\xE3\x81\x82");
        CHECK(runs.size() == 2 && runs[0].face == latin && runs[1].face == japanese, "A あ: Latin, then Japanese");
    }

    // No vertical form anywhere: the CJK face (the one 一 is drawn with) has none either. As before: no vertical form,
    // the Latin face (drawn sideways).
    {
        CHECK(backend.add_book_font("Japanese NoVert", 400, false, read_file(TWT_FIXTURES "/NotoSerifCJKjp-subset-novert.otf")),
              "Japanese NoVert loads");
        const int noVert = backend.create_font("\"Latin\", \"Japanese NoVert\"", 16, 400, false, "", m);
        const int noVertUpright = backend.create_font("\"Latin\", \"Japanese NoVert\"", 16, 400, false, "vert=1", m);
        CHECK(!backend.has_vertical_form(noVert, ellipsis), "no vertical form in the list: none");
        const auto runs = backend.shape(noVertUpright, ellipsis);
        CHECK(runs.size() == 1 && runs[0].face == latin, "no vertical form in the list: the Latin face");
    }

    // The Japanese font first: its own vertical form, as before.
    {
        const int jp = backend.create_font("\"Japanese\", \"Latin\"", 16, 400, false, "", m);
        CHECK(backend.has_vertical_form(jp, ellipsis), "Japanese first: its vertical form");
    }

    std::printf("%d failure(s)\n", g_failed);
    return g_failed ? 1 : 0;
}
