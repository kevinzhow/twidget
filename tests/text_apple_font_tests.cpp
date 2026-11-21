#include "text_backend.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int g_failed = 0, g_checks = 0;
#define CHECK(cond, ...)                                               \
    do {                                                               \
        g_checks++;                                                    \
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

static uint16_t read_u16(const std::vector<uint8_t>& b, size_t at) {
    return at + 2 <= b.size() ? uint16_t((uint16_t(b[at]) << 8) | b[at + 1]) : uint16_t(0);
}

static uint32_t read_u32(const std::vector<uint8_t>& b, size_t at) {
    return at + 4 <= b.size()
               ? (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) | (uint32_t(b[at + 2]) << 8) | uint32_t(b[at + 3])
               : 0u;
}

static void write_u32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    b[at] = uint8_t(v >> 24);
    b[at + 1] = uint8_t(v >> 16);
    b[at + 2] = uint8_t(v >> 8);
    b[at + 3] = uint8_t(v);
}

static std::vector<uint8_t> make_ttc(const std::vector<uint8_t>& f0, const std::vector<uint8_t>& f1) {
    constexpr uint32_t kTtcfVersion1 = 0x00010000u;
    constexpr uint32_t kFaceCount = 2u;
    std::vector<uint8_t> out(20, 0);
    std::memcpy(out.data(), "ttcf", 4);
    write_u32(out, 4, kTtcfVersion1);
    write_u32(out, 8, kFaceCount);
    auto append_face = [&](const std::vector<uint8_t>& sfnt, size_t headerSlot) {
        while (out.size() % 4 != 0) out.push_back(0);
        const uint32_t base = uint32_t(out.size());
        write_u32(out, headerSlot, base);
        out.insert(out.end(), sfnt.begin(), sfnt.end());
        const uint16_t numTables = read_u16(sfnt, 4);
        for (uint16_t t = 0; t < numTables; t++) {
            const size_t rec = size_t(base) + 12u + 16u * t;
            const uint32_t oldOff = read_u32(out, rec + 8u);
            write_u32(out, rec + 8u, oldOff + base);
        }
    };
    append_face(f0, 12);
    append_face(f1, 16);
    return out;
}

static void test_apple_generic_families() {
    using namespace twtext;
    const auto jaSerif = apple_generic_families("serif", "ja");
    CHECK(jaSerif.size() == 4 && jaSerif[0] == "Hiragino Mincho ProN" && jaSerif[1] == "YuMincho", "ja serif");
    const auto jaSans = apple_generic_families("sans-serif", "ja-JP");
    CHECK(jaSans.size() == 3 && jaSans[0] == "Hiragino Sans" && jaSans[1] == "Hiragino Kaku Gothic ProN", "ja-JP sans-serif");
    const auto jaMono = apple_generic_families("monospace", "ja");
    CHECK(jaMono.size() == 3 && jaMono[0] == "Menlo" && jaMono[1] == "Osaka-Mono", "ja monospace");

    const auto zhCnSerif = apple_generic_families("serif", "zh-CN");
    CHECK(zhCnSerif.size() == 4 && zhCnSerif[0] == "Songti SC" && zhCnSerif[1] == "STSong", "zh-CN serif");
    const auto zhCnSans = apple_generic_families("sans-serif", "zh-CN");
    CHECK(zhCnSans.size() == 3 && zhCnSans[0] == "PingFang SC", "zh-CN sans-serif");
    const auto zhCnMono = apple_generic_families("monospace", "zh-CN");
    CHECK(zhCnMono.size() == 2 && zhCnMono[0] == "Menlo", "zh-CN monospace");

    const auto zhTwSerif = apple_generic_families("serif", "zh-TW");
    CHECK(zhTwSerif.size() == 4 && zhTwSerif[0] == "Songti TC", "zh-TW serif");
    const auto zhHantSans = apple_generic_families("sans-serif", "zh-Hant");
    CHECK(zhHantSans.size() == 2 && zhHantSans[0] == "PingFang TC", "zh-Hant sans-serif");
    const auto zhHantMono = apple_generic_families("monospace", "zh_HK");
    CHECK(zhHantMono.size() == 2 && zhHantMono[0] == "Menlo", "zh_HK monospace");

    const auto koSerif = apple_generic_families("serif", "ko");
    CHECK(koSerif.size() == 3 && koSerif[0] == "AppleMyungjo", "ko serif");
    const auto koSans = apple_generic_families("sans-serif", "ko-KR");
    CHECK(koSans.size() == 2 && koSans[0] == "Apple SD Gothic Neo", "ko-KR sans-serif");
    const auto koMono = apple_generic_families("monospace", "ko");
    CHECK(koMono.size() == 2 && koMono[0] == "Menlo", "ko monospace");

    const auto defSerif = apple_generic_families("serif", "");
    CHECK(defSerif.size() == 3 && defSerif[0] == "Times New Roman" && defSerif[1] == "Georgia", "default serif");
    const auto defSans = apple_generic_families("sans-serif", "");
    CHECK(defSans.size() == 3 && defSans[0] == "Helvetica Neue", "default sans-serif");
    const auto defMono = apple_generic_families("monospace", "");
    CHECK(defMono.size() == 2 && defMono[0] == "Menlo", "default monospace");

    const auto cursive = apple_generic_families("cursive", "ja");
    CHECK(cursive.size() == 2 && cursive[0] == "Snell Roundhand", "cursive");
    const auto fantasy = apple_generic_families("fantasy", "");
    CHECK(fantasy.size() == 2 && fantasy[0] == "Papyrus", "fantasy");
    const auto emoji = apple_generic_families("emoji", "ja");
    CHECK(emoji.size() == 1 && emoji[0] == "Apple Color Emoji", "emoji");
    CHECK(apple_generic_families("Helvetica Neue", "").empty(), "non-generic returns empty");
}

static void test_apple_fallback_languages() {
    using namespace twtext;
    CHECK(apple_fallback_languages("ja", "ja-JP") == (std::vector<std::string>{"ja-JP", "ja"}), "ja + ja-JP");
    CHECK(apple_fallback_languages("zh_CN", "") == (std::vector<std::string>{"zh-CN", "zh-Hans"}), "zh_CN -> zh-CN, zh-Hans");
    CHECK(apple_fallback_languages("ja", "en-US,ja-JP") == (std::vector<std::string>{"en-US", "ja-JP", "ja"}), "ja + en-US,ja-JP");
    CHECK(apple_fallback_languages("ja-JP", "ja_jp; en-US") == (std::vector<std::string>{"ja-jp", "en-US"}), "case-insensitive dedup + semicolon");
    CHECK(apple_fallback_languages("zh_TW", "") == (std::vector<std::string>{"zh-TW", "zh-Hant"}), "zh_TW -> zh-TW, zh-Hant");
    CHECK(apple_fallback_languages("", "").empty(), "empty inputs");
}

static void test_apple_weight_and_scoring() {
    using namespace twtext;
    CHECK(apple_ct_weight_to_css(-1.0f) == 100, "clamp low");
    CHECK(apple_ct_weight_to_css(-0.8f) == 100, "100");
    CHECK(apple_ct_weight_to_css(-0.6f) == 200, "200");
    CHECK(apple_ct_weight_to_css(-0.4f) == 300, "300");
    CHECK(apple_ct_weight_to_css(0.0f) == 400, "400");
    CHECK(apple_ct_weight_to_css(0.23f) == 500, "500");
    CHECK(apple_ct_weight_to_css(0.3f) == 600, "600");
    CHECK(apple_ct_weight_to_css(0.4f) == 700, "700");
    CHECK(apple_ct_weight_to_css(0.56f) == 800, "800");
    CHECK(apple_ct_weight_to_css(0.62f) == 900, "900");
    CHECK(apple_ct_weight_to_css(1.0f) == 900, "clamp high");
    CHECK(apple_ct_weight_to_css(-0.2f) == 350, "midpoint interpolation");

    const AppleFaceTraits regular{400, false, false, false, 0.0f};
    const AppleFaceTraits bold{700, false, false, false, 0.0f};
    const AppleFaceTraits italic{400, true, false, false, 0.0f};
    const AppleFaceTraits condensedBold{700, false, true, false, -0.2f};
    const AppleFaceTraits w3{300, false, false, false, 0.0f};
    const AppleFaceTraits w6{600, false, false, false, 0.0f};

    CHECK(apple_score_face(bold, 700, false) < apple_score_face(condensedBold, 700, false), "Bold beats CondensedBold");
    CHECK(apple_score_face(italic, 400, true) < apple_score_face(regular, 400, true), "exact italic beats upright");
    CHECK(apple_score_face(bold, 700, false) < apple_score_face(regular, 700, false), "closest weight wins");
    CHECK(apple_score_face(w3, 400, true) < apple_score_face(w6, 400, true), "in-family closest weight when italic missing");
    CHECK(apple_score_face(w6, 700, true) < apple_score_face(w3, 700, true), "in-family W6 beats W3 for bold italic");
}

static void test_find_ttc_face_index() {
    using namespace twtext;
    const std::vector<uint8_t> otf = read_file(TWT_FIXTURES "/NotoSerifCJKjp-subset.otf");
    const std::vector<uint8_t> woff = read_file(TWT_FIXTURES "/Pacifico.woff");
    CHECK(!otf.empty() && !woff.empty(), "fixture fonts readable");
    TextBackend backend;
    twlite::FontMetrics m{};
    CHECK(backend.add_book_font("PacificoFixture", 400, false, woff), "unpack Pacifico.woff via OTS");
    const int fid = backend.create_font("\"PacificoFixture\"", 16, 400, false, "", m);
    const auto* font = backend.font(fid);
    CHECK(font && font->face, "PacificoFixture face created");
    const std::vector<uint8_t> ttf = font && font->face ? face_bytes(face_info(font->face).id) : std::vector<uint8_t>{};
    CHECK(!ttf.empty(), "unpacked Pacifico SFNT bytes");

    if (!otf.empty() && !ttf.empty()) {
        const std::vector<uint8_t> ttc = make_ttc(otf, ttf);
        CHECK(find_ttc_face_index(ttc.data(), ttc.size(), "NotoSerifCJKjp-Regular") == 0, "face 0 by PostScript name");
        CHECK(find_ttc_face_index(ttc.data(), ttc.size(), "Pacifico-Regular") == 1, "face 1 by PostScript name");
        CHECK(find_ttc_face_index(ttc.data(), ttc.size(), "NoSuchPostScript", "Pacifico Regular") == 1, "face 1 by full name fallback");
        CHECK(find_ttc_face_index(ttc.data(), ttc.size(), "NoSuchPostScript", "NoSuchFull") == 0, "unknown returns 0");
        CHECK(find_ttc_face_index(otf.data(), otf.size(), "NotoSerifCJKjp-Regular") == 0, "single-face returns 0");
    }
}

static std::vector<uint8_t> patch_hhea_leading(const std::vector<uint8_t>& src, int16_t ascent, int16_t descent, int16_t lineGap) {
    std::vector<uint8_t> d = src;
    auto put16 = [&](size_t at, int16_t v) {
        d[at] = uint8_t((uint16_t(v) >> 8) & 0xFFu);
        d[at + 1] = uint8_t(uint16_t(v) & 0xFFu);
    };
    const uint16_t ntables = read_u16(d, 4);
    for (uint16_t i = 0; i < ntables; i++) {
        const size_t rec = 12u + 16u * i;
        if (rec + 16u > d.size()) break;
        const uint32_t off = read_u32(d, rec + 8u);
        const uint32_t len = read_u32(d, rec + 12u);
        if (std::memcmp(&d[rec], "hhea", 4) == 0 && off + 10u <= d.size()) {
            put16(off + 4u, ascent);
            put16(off + 6u, descent);
            put16(off + 8u, lineGap);
        } else if (std::memcmp(&d[rec], "OS/2", 4) == 0 && off + 78u <= d.size()) {
            const uint16_t sel = uint16_t(read_u16(d, off + 62u) & ~uint16_t(128u));
            put16(off + 62u, int16_t(sel));
            put16(off + 68u, ascent);
            put16(off + 70u, descent);
            put16(off + 72u, lineGap);
        } else {
            continue;
        }
        uint32_t sum = 0;
        for (uint32_t p = 0; p < ((len + 3u) & ~3u) && off + p < d.size(); p += 4u) {
            uint32_t w = 0;
            for (int k = 0; k < 4; k++) w = (w << 8) | (off + p + k < off + len ? d[off + p + k] : 0u);
            sum += w;
        }
        write_u32(d, rec + 4u, sum);
    }
    return d;
}

static void test_symmetric_leading_metrics() {
    using namespace twtext;
    const std::vector<uint8_t> otf = read_file(TWT_FIXTURES "/NotoSerifCJKjp-subset.otf");
    const std::vector<uint8_t> woff = read_file(TWT_FIXTURES "/Pacifico.woff");
    CHECK(!otf.empty() && !woff.empty(), "leading fixtures readable");
    if (otf.empty() || woff.empty()) return;

    // Mimic macOS Hiragino (upem=1000, hhea ascent=880, descent=-120, lineGap=500):
    // half-leading (250/1000) splits symmetrically into ascent (1130) and descent (370).
    const std::vector<uint8_t> hiraginoLike = patch_hhea_leading(otf, 880, -120, 500);
    const std::vector<uint8_t> zeroLead = patch_hhea_leading(otf, 880, -120, 0);
    TextBackend backend;
    CHECK(backend.add_book_font("LatinPrimary", 400, false, woff), "add LatinPrimary");
    CHECK(backend.add_book_font("LeadingCJK", 400, false, hiraginoLike), "add LeadingCJK");
    CHECK(backend.add_book_font("ZeroLeadCJK", 400, false, zeroLead), "add ZeroLeadCJK");

    twlite::FontMetrics m{};
    const int cjkId = backend.create_font("\"LeadingCJK\"", 16.0f, 400, false, "", m);
    CHECK(cjkId > 0, "LeadingCJK font created");
    CHECK(std::fabs(m.ascent - 18.08f) < 0.02f, "ascent includes half-leading (got %.4f)", m.ascent);
    CHECK(std::fabs(m.descent - 5.92f) < 0.02f, "descent includes half-leading (got %.4f)", m.descent);
    CHECK(std::fabs(m.height - 24.0f) < 0.02f && std::fabs((m.ascent + m.descent) - m.height) < 0.01f,
          "ascent+descent == height == 24.0 (got %.4f + %.4f vs %.4f)", m.ascent, m.descent, m.height);
    CHECK(std::fabs(m.leading - 8.0f) < 0.02f, "leading reports scaled lineGap (got %.4f)", m.leading);
    CHECK(std::fabs(m.em_ascent - 14.078f) < 0.05f, "OS/2 em_ascent unaffected by leading (got %.4f)", m.em_ascent);

    twlite::FontMetrics zeroM{};
    const int zeroId = backend.create_font("\"ZeroLeadCJK\"", 16.0f, 400, false, "", zeroM);
    CHECK(zeroId > 0 && std::fabs(zeroM.leading) < 1e-4f, "ZeroLeadCJK reports leading == 0 (got %.4f)", zeroM.leading);

    twlite::FontMetrics stackM{};
    const int stackId = backend.create_font("\"LatinPrimary\", \"LeadingCJK\"", 16.0f, 400, false, "", stackM);
    CHECK(stackId > 0, "stack font created");
    const auto extents = backend.fallback_extents(stackId, "\xE3\x81\x82");
    CHECK(extents.size() == 1, "fallback_extents returned 1 CJK face (got %zu)", extents.size());
    if (!extents.empty()) {
        CHECK(std::fabs(extents[0].first - 18.08f) < 0.02f && std::fabs(extents[0].second - 5.92f) < 0.02f,
              "fallback_extents includes half-leading (got %.4f, %.4f)", extents[0].first, extents[0].second);
    }
}

#ifdef __APPLE__
static bool is_sfnt(const std::vector<uint8_t>& b) {
    return b.size() >= 4 && ((b[0] == 0 && b[1] == 1 && b[2] == 0 && b[3] == 0) || std::memcmp(b.data(), "OTTO", 4) == 0 ||
                             std::memcmp(b.data(), "true", 4) == 0);
}

static void test_macos_coretext_discovery() {
    using namespace twtext;
    CHECK(font_family_available("Hiragino Mincho ProN"), "Hiragino Mincho ProN available");
    CHECK(font_family_available("Hiragino Sans"), "Hiragino Sans available");
    CHECK(font_family_available("Menlo"), "Menlo available");
    CHECK(!font_family_available("NoSuchMacFont12345"), "missing font rejected");

    TextOptions jaOpts;
    jaOpts.language = "ja";
    jaOpts.locales = "ja-JP";
    TextBackend jaBackend(jaOpts);
    twlite::FontMetrics m{};

    const int serifId = jaBackend.create_font("serif", 16, 400, false, "", m);
    const auto* serifFont = jaBackend.font(serifId);
    CHECK(serifFont && serifFont->face, "ja serif resolved");
    if (serifFont && serifFont->face) {
        const std::string& fam = face_info(serifFont->face).family;
        CHECK(fam == "Hiragino Mincho ProN" || fam == "YuMincho", "ja serif family=%s", fam.c_str());
    }

    const int sansId = jaBackend.create_font("sans-serif", 16, 400, false, "", m);
    const auto* sansFont = jaBackend.font(sansId);
    CHECK(sansFont && sansFont->face, "ja sans-serif resolved");
    if (sansFont && sansFont->face) {
        const std::string& fam = face_info(sansFont->face).family;
        CHECK(fam == "Hiragino Sans" || fam == "Hiragino Kaku Gothic ProN", "ja sans family=%s", fam.c_str());
    }

    const int monoId = jaBackend.create_font("monospace", 16, 400, false, "", m);
    const auto* monoFont = jaBackend.font(monoId);
    CHECK(monoFont && monoFont->face, "ja monospace resolved");
    if (monoFont && monoFont->face) {
        const std::string& fam = face_info(monoFont->face).family;
        CHECK(fam == "Menlo" || fam == "SF Mono", "ja monospace family=%s", fam.c_str());
    }

    const int jaHnId = jaBackend.create_font("Helvetica Neue", 16, 400, false, "", m);
    const auto* jaHnFont = jaBackend.font(jaHnId);
    CHECK(jaHnFont && jaHnFont->face && face_info(jaHnFont->face).family == "Helvetica Neue",
          "explicit Helvetica Neue keeps its family under ja");

    TextBackend defaultBackend;
    const int hnRegId = defaultBackend.create_font("Helvetica Neue", 16, 400, false, "", m);
    const int hnBoldId = defaultBackend.create_font("Helvetica Neue", 16, 700, false, "", m);
    const int hnItalId = defaultBackend.create_font("Helvetica Neue", 16, 400, true, "", m);
    const auto* hnReg = defaultBackend.font(hnRegId);
    const auto* hnBold = defaultBackend.font(hnBoldId);
    const auto* hnItal = defaultBackend.font(hnItalId);
    CHECK(hnReg && hnReg->face && hnBold && hnBold->face && hnItal && hnItal->face, "Helvetica Neue variants resolved");
    if (hnReg && hnReg->face && hnBold && hnBold->face && hnItal && hnItal->face) {
        const FaceInfo& rInfo = face_info(hnReg->face);
        const FaceInfo& bInfo = face_info(hnBold->face);
        const FaceInfo& iInfo = face_info(hnItal->face);
        CHECK(rInfo.file != bInfo.file || rInfo.index != bInfo.index, "regular vs bold distinct (%s:%d vs %s:%d)",
              rInfo.file.c_str(), rInfo.index, bInfo.file.c_str(), bInfo.index);
        CHECK(rInfo.file != iInfo.file || rInfo.index != iInfo.index, "regular vs italic distinct (%s:%d vs %s:%d)",
              rInfo.file.c_str(), rInfo.index, iInfo.file.c_str(), iInfo.index);
        CHECK(bInfo.weightClass >= 600 && !bInfo.italic, "bold weightClass=%d italic=%d", bInfo.weightClass, int(bInfo.italic));
        CHECK(iInfo.italic, "italic face has italic=true");
        CHECK(is_sfnt(face_bytes(bInfo.id)), "face_bytes extracts standalone SFNT from .ttc");
    }

    const int tnrId = jaBackend.create_font("Times New Roman", 16, 400, false, "", m);
    const auto* tnrFont = jaBackend.font(tnrId);
    CHECK(tnrFont && tnrFont->face, "Times New Roman resolved");
    if (tnrFont && tnrFont->face) {
        const auto cjkRuns = jaBackend.shape(tnrId, "\xE6\xBC\xA2\xE5\xAD\x97\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86");
        CHECK(!cjkRuns.empty(), "Japanese shaped via cascade");
        bool allNonZero = !cjkRuns.empty();
        bool usedFallbackFace = false;
        for (const auto& run : cjkRuns) {
            if (run.face != tnrFont->face) usedFallbackFace = true;
            for (uint32_t g : run.glyphs)
                if (g == 0) allNonZero = false;
        }
        CHECK(usedFallbackFace && allNonZero, "Japanese fell back to non-tofu CJK face");

        const auto emojiRuns = jaBackend.shape(tnrId, "\xF0\x9F\x8F\x86");
        CHECK(!emojiRuns.empty(), "emoji shaped");
        bool gotAppleEmoji = false;
        for (const auto& run : emojiRuns) {
            if (run.face && face_info(run.face).family == "Apple Color Emoji") gotAppleEmoji = true;
        }
        CHECK(gotAppleEmoji, "trophy emoji fell back to Apple Color Emoji");
    }

    // Fallback Japanese from Helvetica Neue must report Hiragino's full height (ascent + descent + leading).
    twlite::FontMetrics hnM{}, hiraM{}, stackM{};
    const int hnId = jaBackend.create_font("\"Helvetica Neue\"", 16, 400, false, "", hnM);
    const int hiraId = jaBackend.create_font("\"Hiragino Sans\"", 16, 400, false, "", hiraM);
    const int stackId = jaBackend.create_font("\"Helvetica Neue\", \"Hiragino Sans\"", 16, 400, false, "", stackM);
    CHECK(hnId > 0 && hiraId > 0 && stackId > 0, "Helvetica Neue + Hiragino Sans fonts created");
    const auto fb = jaBackend.fallback_extents(stackId, "\xE3\x82\x84\xE3\x81\x8C\xE3\x81\xA6");
    CHECK(!fb.empty(), "fallback_extents non-empty for Japanese in Helvetica Neue stack");
    if (!fb.empty()) {
        const float fbHeight = fb[0].first + fb[0].second;
        CHECK(std::fabs(fbHeight - hiraM.height) < 0.25f && fbHeight > hnM.height,
              "Hiragino fallback height %.3f matches direct %.3f and exceeds Latin %.3f", fbHeight, hiraM.height, hnM.height);
    }
}
#endif

int main() {
    test_apple_generic_families();
    test_apple_fallback_languages();
    test_apple_weight_and_scoring();
    test_find_ttc_face_index();
    test_symmetric_leading_metrics();
#ifdef __APPLE__
    test_macos_coretext_discovery();
#endif
    std::printf("text_apple_font_tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
