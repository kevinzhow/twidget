// twidget::text: fonts and text measurement for TwidgetCore's Backend, shared by twidget_flutter's cards and hosts
// that lay out documents of their own (an EPUB reader). The system font matcher (fontconfig on Linux; on Android the
// NDK's AFontMatcher and /system/etc/fonts.xml, the files Flutter's Skia also gets), the font tables (metrics) and
// HarfBuzz (shaping, unhinted advances), plus a book's own fonts (@font-face), which come first.
//
// Also provides book fonts (@font-face), per-byte advances, the fallback extents of `line-height: normal`, shaped
// glyph runs and a font cache. Images and canvases are a host's:
// load_image refuses and the canvas calls do nothing here; twidget_flutter's card backend overrides them.
#pragma once

#include "backend.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct hb_font_t;
struct hb_feature_t;

namespace twtext {

/// One font file (or font in memory) at one weight / slant, shared by every document (immutable once made).
struct Face;

/// A face's identity for a painter: an id for this process, the file (and collection index) it was made from, or no
/// file for a book's font (face_bytes has it), the variation coordinates it is set to, and the weight class and italic
/// flag of the file itself.
struct FaceInfo {
    int id = 0;
    std::string file;
    int index = 0;
    std::vector<std::pair<std::string, float>> axes;  // every axis of a variable font, at the face's coordinates
    int weightClass = 400;
    bool italic = false;
    std::string family;  // what the font matcher (or the book's @font-face) calls it
};
const FaceInfo& face_info(const Face* face);
/// The face's font as one OpenType file (a collection's member taken out of it). Empty when unknown or unreadable.
std::vector<uint8_t> face_bytes(int faceId);
/// The HarfBuzz font of a face, at a scale of its units per em (for a painter that shapes, as the reader's tools do).
hb_font_t* face_hb_font(const Face* face);
float face_upem(const Face* face);

/// A CSS family name resolves to a system font of its own: a generic name, or a family or alias the system knows.
bool font_family_available(const std::string& family);

/// Android's fonts.xml: a font of a fallback family (one with no name) that stands in for the family's others when
/// the text's family is `fallbackFor` (e.g. NotoSerifCJK, fallbackFor="serif", in the family of NotoSansCJK). Skia's
/// font manager (Flutter, Chromium's WebView) honours it; the NDK's AFontMatcher does not (observed on API 36: serif
/// Japanese text matched NotoSansCJK), so the fallback below substitutes it itself.
struct FallbackFor {
    std::string file;  // as fonts.xml writes it (a file name in /system/fonts)
    int index = 0;
    int weight = 400;
    bool italic = false;
    std::vector<std::pair<std::string, float>> axes;  // its axis tags and style values
};
/// A fonts.xml fallback family: its fonts (file, collection index) and those standing in for named families.
struct FallbackFamily {
    std::vector<std::pair<std::string, int>> fonts;
    std::map<std::string, std::vector<FallbackFor>> fallbackFor;  // by lower-case family
};
/// The fallback families of a fonts.xml text, in its order.
std::vector<FallbackFamily> parse_fallback_families(const std::string& xml);
/// The font that stands in for `file` (a path or a file name; collection `index`) when the text's family is `family`:
/// the fallbackFor font of the fallback family holding `file`, closest in style, then weight. False when none does.
bool fallback_for(const std::vector<FallbackFamily>& families, const std::string& file, int index, const std::string& family,
                  int weight, bool italic, FallbackFor& out);

/// What a backend matches fonts for. Faces, matches and fallbacks are cached process-wide per these options, so
/// backends with different options can share a process.
struct TextOptions {
    /// The language generic families and fallbacks prefer fonts for: fontconfig's FC_LANG on Linux ("ja": the Japanese
    /// fonts, as Chromium matches for lang="ja"). Empty: none.
    std::string language;
    /// Android: the locale list of the NDK's font matcher for characters the list's fonts lack ("ja-JP": the JP forms
    /// of Han characters). Empty: the system's.
    std::string locales;
    /// Android: those characters fall back through the list's generic family (serif: the serif CJK fonts), as Skia
    /// does for a TextStyle with that family; false: always through sans-serif.
    bool generic_fallback = false;
    /// create_font with the same arguments gives the font made before (its id and its widths); false: a new font each
    /// time.
    bool reuse_fonts = false;
};

/// Milliseconds spent in the font matcher and face setup so far, every backend (diagnostics).
double font_setup_ms();

class TextBackend : public twlite::Backend {
public:
    /// Consecutive characters of a text run that one face measured: the face and the run's UTF-16 length.
    struct Segment {
        Face* face = nullptr;
        int utf16 = 0;
    };
    struct Font {
        std::string family;                  // what the font matcher matched (the name Flutter asks Skia for)
        std::vector<std::string> fallbacks;  // the matched family of each name in the CSS list
        float size = 0;
        int weight = 400;
        bool italic = false;
        std::string features;
        Face* face = nullptr;
        std::vector<Face*> bookFaces;        // the book's faces named by the list, in its order, before the system's
        std::vector<hb_feature_t>* hbFeatures = nullptr;
        std::unordered_map<std::string, float> widths;
    };
    /// A shaped run: glyphs of one face, positions in CSS px from the run's origin (y down).
    struct GlyphRun {
        Face* face = nullptr;
        std::vector<uint32_t> glyphs;
        std::vector<float> x, y;
    };

    explicit TextBackend(TextOptions options = {});
    ~TextBackend() override;
    TextBackend(const TextBackend&) = delete;
    TextBackend& operator=(const TextBackend&) = delete;

    int create_font(const std::string& families, float size_px, int weight, bool italic, const std::string& features, twlite::FontMetrics& out) override;
    void delete_font(int font) override;
    float text_width(int font, const std::string& text) override;
    /// `text` shaped as one run in `font`, split by face as text_width splits it: `prefix[b]` is the advance before
    /// byte `b` (text.size() + 1 entries). A glyph's advance goes to the first character of its HarfBuzz cluster.
    void text_advances(int font, const std::string& text, std::vector<float>& prefix);
    /// No images here: a host loads its own (a card backend overrides this).
    bool load_image(int, const std::string&, float&, float&) override { return false; }

    /// The faces `text` is measured with in `font`, in order (text_width shapes each segment with its face).
    std::vector<Segment> segments(int font, const std::string& text);
    /// The ascent and descent (CSS px, unrounded) of each face other than the font's own that `text` is measured with
    /// in `font`: fallback faces, which a line of `line-height: normal` makes room for (CSS Inline 3 §4.2; Blink
    /// includes the used fonts there). Empty when the font's own face draws all of it.
    std::vector<std::pair<float, float>> fallback_extents(int font, const std::string& text);
    /// `text` shaped as text_width measures it, for a painter.
    std::vector<GlyphRun> shape(int font, const std::string& text);

    /// A book's font (@font-face): `family` at `weight` (100..900) and style, from the font file's bytes (TrueType,
    /// OpenType or a collection's first font; WOFF/WOFF2 are refused). Fonts made after it see it. False when the
    /// bytes are no font.
    bool add_book_font(const std::string& family, int weight, bool italic, std::vector<uint8_t> bytes);

    const std::map<int, Font>& fonts() const { return fonts_; }
    /// Counts the fonts made: changes whenever fonts() gains one.
    int64_t fonts_version() const { return fontsVersion_; }
    const TextOptions& options() const { return options_; }
    const Font* font(int id) const {
        auto it = fonts_.find(id);
        return it == fonts_.end() ? nullptr : &it->second;
    }

private:
    TextOptions options_;
    int optionsId_ = 0;  // the caches' key for options_
    int64_t fontsVersion_ = 0;
    struct BookFont {
        int weight = 400;
        bool italic = false;
        Face* face = nullptr;
    };
    std::map<std::string, std::vector<BookFont>> bookFonts_;  // by lower-case family
    std::vector<std::unique_ptr<Face>> ownFaces_;
    std::map<int, Font> fonts_;
    std::map<std::string, std::pair<int, twlite::FontMetrics>> fontKeys_;
    int create_font_uncached(const std::string& families, float size_px, int weight, bool italic, const std::string& features, twlite::FontMetrics& out);
    int nextFont_ = 0;
};

} // namespace twtext
