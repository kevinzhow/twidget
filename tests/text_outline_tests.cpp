// twidget::text's glyph outlines for GPU text (encode_glyph_outline, HarfBuzz hb-gpu): the glyphs a shaped run names
// encode to blobs whose ink box is the glyph's, a space has no ink, an unknown face is refused, and the encoding is
// the same every time and from another thread. Without hb-gpu in the build, only that it says so.
#include "text_backend.h"

#include <cstdio>
#include <string>
#include <thread>
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

int main() {
    using namespace twtext;
    if (!glyph_outlines_available()) {
        twtext::GlyphOutline o;
        CHECK(!encode_glyph_outline(1, 1, o), "no hb-gpu: nothing encodes");
        std::printf("text_outline_tests: no hb-gpu in this build; %d checks, %d failed\n", g_checks, g_failed);
        return g_failed ? 1 : 0;
    }
    TextBackend backend;
    twlite::FontMetrics m{};
    const int font = backend.create_font("sans-serif", 16, 400, false, "", m);
    CHECK(font >= 0, "a font");
    // Latin, a space and (when the system has a CJK font) a Han character.
    const std::string text = "Ag \xE6\xB0\xB8";
    const auto runs = backend.shape(font, text);
    CHECK(!runs.empty(), "shaped");
    int inked = 0, empty = 0;
    std::vector<std::pair<int, uint32_t>> glyphs;
    for (const auto& run : runs) {
        const int face = face_info(run.face).id;
        CHECK(face_upem_of(face) == face_upem(run.face), "upem by id");
        for (uint32_t g : run.glyphs) {
            glyphs.push_back({face, g});
            GlyphOutline o;
            CHECK(encode_glyph_outline(face, g, o), "face %d glyph %u encodes", face, g);
            if (o.blob.empty()) {
                empty++;
                continue;
            }
            inked++;
            CHECK(o.blob.size() % 8 == 0, "whole texels: %zu bytes", o.blob.size());
            CHECK(o.maxX > o.minX && o.maxY > o.minY, "an ink box: %d %d %d %d", o.minX, o.minY, o.maxX, o.maxY);
            const float upem = face_upem(run.face);
            CHECK(o.maxY <= int32_t(upem * 1.5f) && o.minY >= -int32_t(upem), "within the em: %d..%d (upem %g)", o.minY, o.maxY, double(upem));
            GlyphOutline again;
            encode_glyph_outline(face, g, again);
            CHECK(again.blob == o.blob && again.minX == o.minX && again.maxY == o.maxY, "the same twice");
        }
    }
    CHECK(inked >= 2, "A and g have ink (%d)", inked);
    CHECK(empty >= 1, "the space has none (%d)", empty);
    // From another thread (the engine isolate's): the same blobs.
    std::vector<GlyphOutline> here(glyphs.size()), there(glyphs.size());
    for (size_t i = 0; i < glyphs.size(); i++) encode_glyph_outline(glyphs[i].first, glyphs[i].second, here[i]);
    std::thread t([&] {
        for (size_t i = 0; i < glyphs.size(); i++) encode_glyph_outline(glyphs[i].first, glyphs[i].second, there[i]);
    });
    t.join();
    for (size_t i = 0; i < glyphs.size(); i++) CHECK(here[i].blob == there[i].blob, "glyph %zu the same on another thread", i);
    GlyphOutline none;
    CHECK(!encode_glyph_outline(0, 1, none) && !encode_glyph_outline(1 << 20, 1, none), "unknown faces are refused");
    std::printf("text_outline_tests: %d glyphs (%d inked); %d checks, %d failed\n", int(glyphs.size()), inked, g_checks, g_failed);
    return g_failed ? 1 : 0;
}
