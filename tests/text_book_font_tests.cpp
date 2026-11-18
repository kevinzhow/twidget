// twidget::text's book fonts (TextBackend::add_book_font, an EPUB's @font-face): OpenType, TrueType, WOFF and WOFF2
// files load, a family that names one measures with it, and the face's bytes a painter loads are a plain OpenType
// file (what Skia, FreeType and HarfBuzz read). Bytes that are no font are refused.
// Fixtures: tests/fixtures/fonts (Pacifico.woff, Macondo.woff2: SIL Open Font License, the files beside them).
#include "text_backend.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int g_failed = 0, g_checks = 0;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        g_checks++;                                       \
        if (!(cond)) {                                    \
            g_failed++;                                   \
            std::printf("FAIL %s:%d: %s ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                     \
            std::printf("\n");                            \
        }                                                 \
    } while (0)

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

static bool sfnt(const std::vector<uint8_t>& b) {
    return b.size() >= 4 && ((b[0] == 0 && b[1] == 1 && b[2] == 0 && b[3] == 0) || std::memcmp(b.data(), "OTTO", 4) == 0 ||
                             std::memcmp(b.data(), "true", 4) == 0);
}

int main() {
    using namespace twtext;
    TextBackend backend;
    twlite::FontMetrics m{};

    // A plain OpenType/TrueType file: a system face's own bytes.
    const int sys = backend.create_font("sans-serif", 16, 400, false, "", m);
    const std::vector<uint8_t> plain = sys > 0 && backend.fonts().at(sys).face ? face_bytes(face_info(backend.fonts().at(sys).face).id) : std::vector<uint8_t>{};
    CHECK(sfnt(plain), "a system face's bytes are an OpenType file");

    struct Case {
        const char* family;
        std::vector<uint8_t> bytes;
    } cases[] = {
        {"Book Plain", plain},
        {"Book Woff", read_file(TWT_FIXTURES "/Pacifico.woff")},
        {"Book Woff2", read_file(TWT_FIXTURES "/Macondo.woff2")},
    };
    for (auto& c : cases) {
        CHECK(!c.bytes.empty(), "%s: fixture read", c.family);
        CHECK(backend.add_book_font(c.family, 400, false, c.bytes), "%s: loads", c.family);
        const int font = backend.create_font(std::string("\"") + c.family + "\", serif", 16, 400, false, "", m);
        const auto& f = backend.fonts().at(font);
        CHECK(!f.bookFaces.empty() && f.face == f.bookFaces.front(), "%s: the family measures with the book's face", c.family);
        if (f.bookFaces.empty()) continue;
        CHECK(sfnt(face_bytes(face_info(f.bookFaces.front()).id)), "%s: the painter gets an OpenType file", c.family);
        CHECK(!backend.shape(font, "Hello").empty(), "%s: shapes text", c.family);
    }

    // A ligature is one cluster: text_advances has no advance between its characters (NaN), so that a piece of a run
    // cut there is measured alone. Pacifico's ff, also formed across a soft hyphen (HarfBuzz skips default ignorables).
    {
        const int font = backend.create_font("\"Book Woff\", serif", 16, 400, false, "", m);
        std::vector<float> p;
        backend.text_advances(font, "aff", p);
        CHECK(p.size() == 4 && p[0] == 0 && p[1] > 0 && std::isnan(p[2]) && p[3] > p[1], "ff: one cluster");
        backend.text_advances(font, "af\xC2\xAD" "fr", p);
        CHECK(p.size() == 7 && p[1] > 0 && std::isnan(p[2]) && std::isnan(p[3]) && std::isnan(p[4]) && p[5] > p[1] && p[6] > p[5],
              "f<soft hyphen>f: one cluster");
    }

    // Not fonts: a WOFF header over garbage, and text.
    std::vector<uint8_t> bad(400, 0x5a);
    std::memcpy(bad.data(), "wOFF", 4);
    CHECK(!backend.add_book_font("Bad Woff", 400, false, bad), "a broken WOFF is refused");
    const std::string text = "<html>not a font</html>";
    CHECK(!backend.add_book_font("Bad Text", 400, false, std::vector<uint8_t>(text.begin(), text.end())), "text is refused");

    {
        TextOptions options;
        options.reuse_fonts = true;
        TextBackend shared(options);
        const std::string families = "Shared Book Face, serif";
        const int system_font = shared.create_font(families, 16, 400, false, "", m);
        int embedded_font = 0, embedded_face = 0;
        {
            TextBackend::BookFonts book(shared);
            TextBackend::BookFonts other(shared);
            CHECK(book.create_font(families, 16, 400, false, "", m) == system_font, "shares cached system fonts");
            CHECK(book.add("Shared Book Face", 400, false, cases[1].bytes), "registers a face in the book");
            embedded_font = book.create_font(families, 16, 400, false, "", m);
            embedded_face = face_info(shared.fonts().at(embedded_font).face).id;
            CHECK(embedded_font != system_font, "registration replaces the cached system match");
            CHECK(other.create_font(families, 16, 400, false, "", m) == system_font, "another open book uses the system face");
            CHECK(book.add("Shared Book Face", 400, false, cases[1].bytes), "rereads a shared stylesheet");
            CHECK(book.create_font(families, 16, 400, false, "", m) == embedded_font, "duplicate registration keeps the font handle");
        }
        CHECK(!shared.font(embedded_font) && face_bytes(embedded_face).empty(), "the closed book's font and face are released");
        CHECK(shared.create_font(families, 16, 400, false, "", m) == system_font, "system font survives closing the book");
    }

    std::printf("text_book_font_tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
