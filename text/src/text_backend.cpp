#include "text_backend.h"

#ifdef __ANDROID__
#include <dlfcn.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#else
#include <fontconfig/fontconfig.h>
#endif
#include <hb.h>
#include <hb-ot.h>
#if defined(TWT_HB_GPU)
#include <hb-gpu.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <memory>
#include <mutex>
#include <tuple>

namespace twtext {

struct Face {
    FaceInfo info;
    std::string file;                              // empty for a book's font
    std::shared_ptr<std::vector<uint8_t>> bytes;  // a book's font
    int index = 0;
    std::string family;
    hb_font_t* font = nullptr;  // scale = upem: advances in font units
    float upem = 1000;
    // In font units, Qt's choice: OS/2 typo when USE_TYPO_METRICS, else hhea (OS/2 win when hhea is empty).
    float ascent = 0, descent = 0, leading = 0, xHeight = 0;
    float typoAscent = 0, typoDescent = 0;
    bool hasTypo = false;
};

namespace {
std::string xml_attribute(const std::string& tag, const char* name) {
    std::string key = std::string(" ") + name + "=\"";
    size_t at = tag.find(key);
    if (at == std::string::npos) return "";
    size_t end = tag.find('"', at + key.size());
    return end == std::string::npos ? "" : tag.substr(at + key.size(), end - at - key.size());
}
std::string xml_trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
std::string base_name(const std::string& path) {
    size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
} // namespace

std::vector<FallbackFamily> parse_fallback_families(const std::string& xml) {
    std::vector<FallbackFamily> out;
    FallbackFamily* family = nullptr;  // the fallback family being read (none inside a named one)
    for (size_t at = xml.find('<'); at != std::string::npos; at = xml.find('<', at + 1)) {
        if (xml.compare(at, 4, "<!--") == 0) {
            at = xml.find("-->", at);
            if (at == std::string::npos) break;
            continue;
        }
        size_t close = xml.find('>', at);
        if (close == std::string::npos) break;
        std::string tag = xml.substr(at, close - at);
        for (char& c : tag)
            if (c == '\n' || c == '\t' || c == '\r') c = ' ';
        if (tag.rfind("<family ", 0) == 0 || tag == "<family") {  // (not <familyset>)
            family = xml_attribute(tag, "name").empty() ? &out.emplace_back() : nullptr;
        } else if (tag.rfind("</family", 0) == 0 && tag.rfind("</familyset", 0) != 0) {
            family = nullptr;
        } else if (tag.rfind("<font ", 0) == 0 || tag == "<font") {
            size_t end = xml.find("</font>", close);
            if (!family || end == std::string::npos) continue;
            const std::string body = xml.substr(close + 1, end - close - 1);
            FallbackFor f;
            f.file = xml_trim(body.substr(0, body.find('<')));
            f.index = std::atoi(xml_attribute(tag, "index").c_str());
            f.weight = std::atoi(xml_attribute(tag, "weight").c_str());
            if (f.weight <= 0) f.weight = 400;
            f.italic = xml_attribute(tag, "style") == "italic";
            for (size_t a = body.find("<axis"); a != std::string::npos; a = body.find("<axis", a + 1)) {
                const std::string axis = body.substr(a, body.find('>', a) - a);
                const std::string t = xml_attribute(axis, "tag");
                if (t.size() == 4) f.axes.emplace_back(t, float(std::atof(xml_attribute(axis, "stylevalue").c_str())));
            }
            if (f.file.empty()) continue;
            std::string target = xml_attribute(tag, "fallbackFor");
            for (char& c : target) c = char(std::tolower((unsigned char)c));
            if (target.empty()) family->fonts.emplace_back(f.file, f.index);
            else family->fallbackFor[target].push_back(std::move(f));
        }
    }
    return out;
}

bool fallback_for(const std::vector<FallbackFamily>& families, const std::string& file, int index, const std::string& family,
                  int weight, bool italic, FallbackFor& out) {
    std::string key = family;
    for (char& c : key) c = char(std::tolower((unsigned char)c));
    const std::string name = base_name(file);
    for (const FallbackFamily& f : families) {
        if (std::find(f.fonts.begin(), f.fonts.end(), std::make_pair(name, index)) == f.fonts.end()) continue;
        auto it = f.fallbackFor.find(key);
        if (it == f.fallbackFor.end() || it->second.empty()) return false;
        const FallbackFor* best = nullptr;
        int bestScore = 0;
        for (const FallbackFor& c : it->second) {
            const int score = std::abs(c.weight - weight) + (c.italic != italic ? 10000 : 0);
            if (!best || score < bestScore) best = &c, bestScore = score;
        }
        out = *best;
        return true;
    }
    return false;
}

namespace {

std::mutex g_mutex;  // the font matcher, the face setup and the caches below: every backend shares them
double g_setupMs = 0;
// The distinct TextOptions in use (their match-relevant members), by id: what the caches below are keyed by.
std::vector<std::tuple<std::string, std::string, bool>> g_options;
// (file, index, weight, italic) → face
std::map<std::tuple<std::string, int, int, bool>, std::unique_ptr<Face>> g_faces;
struct Resolved {
    Face* face = nullptr;
    std::string family;                  // the family name Flutter asks its font manager for
    std::vector<std::string> fallbacks;  // the family of each name in the CSS list
};
// CSS family list + weight + italic + options → primary face and family names
std::map<std::tuple<std::string, int, bool, int>, Resolved> g_resolved;
// (codepoint, weight, italic, the font's own face, options) → a face that has it
std::map<std::tuple<uint32_t, int, bool, Face*, int>, Face*> g_glyphFallback;
std::vector<Face*> g_faceById;  // FaceInfo::id - 1 → face

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

std::string trim(std::string s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '"' || c == '\''; };
    while (!s.empty() && ws(s.front())) s.erase(s.begin());
    while (!s.empty() && ws(s.back())) s.pop_back();
    return s;
}

/// What the system's font matcher found for a family list (and a codepoint, when one is given).
struct Match {
    std::string file;
    int index = 0;
    std::string family;                // the name Flutter's font manager knows the family by
    std::vector<hb_variation_t> axes;  // the variation axes the system font list sets for this style (Android)
};

#ifdef __ANDROID__
// Android: no fontconfig. Families come from /system/etc/fonts.xml, the file Flutter's font manager (Skia's Android
// one) reads, so a CSS family resolves to the font Flutter paints with: its named families and aliases, the closest
// weight and style. A codepoint the list's fonts lack goes to the system's fallback chain through the NDK's font
// matcher (API 29). AFontMatcher is not used for names: on API 36 it resolves one name at a time and misses some
// (serif-monospace, casual) and every alias.

Face* face_for(const Match& m, int weight, bool italic);

struct SystemFont {
    std::string file;
    int index = 0;
    int weight = 400;
    bool italic = false;
    std::vector<hb_variation_t> axes;
};
struct SystemFamily {
    std::string name;  // as fonts.xml writes it
    std::vector<SystemFont> fonts;
    int weight = 0;    // an alias's fixed weight
};

std::string attribute(const std::string& tag, const char* name) {
    std::string key = std::string(" ") + name + "=\"";
    size_t at = tag.find(key);
    if (at == std::string::npos) return "";
    size_t end = tag.find('"', at + key.size());
    return end == std::string::npos ? "" : tag.substr(at + key.size(), end - at - key.size());
}

/// fonts.xml's named families and aliases, by lower-case name.
const std::map<std::string, SystemFamily>& system_families() {
    static std::map<std::string, SystemFamily> families = [] {
        std::map<std::string, SystemFamily> out;
        std::ifstream in("/system/etc/fonts.xml");
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string xml = ss.str();
        SystemFamily* family = nullptr;  // the named family being read
        for (size_t at = xml.find('<'); at != std::string::npos; at = xml.find('<', at + 1)) {
            if (xml.compare(at, 4, "<!--") == 0) {
                at = xml.find("-->", at);
                if (at == std::string::npos) break;
                continue;
            }
            size_t close = xml.find('>', at);
            if (close == std::string::npos) break;
            std::string tag = xml.substr(at, close - at);
            for (char& c : tag)
                if (c == '\n' || c == '\t' || c == '\r') c = ' ';
            if (tag.rfind("<family", 0) == 0) {
                std::string name = attribute(tag, "name");
                family = name.empty() ? nullptr : &out[lower(name)];
                if (family) family->name = name;
            } else if (tag.rfind("</family", 0) == 0) {
                family = nullptr;
            } else if (tag.rfind("<font ", 0) == 0 || tag == "<font") {
                size_t end = xml.find("</font>", close);
                if (!family || end == std::string::npos) continue;
                SystemFont f;
                f.weight = std::atoi(attribute(tag, "weight").c_str());
                if (f.weight <= 0) f.weight = 400;
                f.italic = attribute(tag, "style") == "italic";
                f.index = std::atoi(attribute(tag, "index").c_str());
                // The file name is the text up to the first child tag (axes).
                std::string body = xml.substr(close + 1, end - close - 1);
                std::string file = trim(body.substr(0, body.find('<')));
                for (size_t a = body.find("<axis"); a != std::string::npos; a = body.find("<axis", a + 1)) {
                    std::string axis = body.substr(a, body.find('>', a) - a);
                    std::string t = attribute(axis, "tag");
                    if (t.size() == 4) f.axes.push_back({hb_tag_from_string(t.c_str(), 4), float(std::atof(attribute(axis, "stylevalue").c_str()))});
                }
                if (!file.empty()) {
                    f.file = "/system/fonts/" + file;
                    family->fonts.push_back(std::move(f));
                }
            } else if (tag.rfind("<alias", 0) == 0) {
                std::string name = attribute(tag, "name"), to = attribute(tag, "to");
                auto target = out.find(lower(to));
                if (name.empty() || target == out.end()) continue;
                SystemFamily alias = target->second;
                alias.name = name;
                alias.weight = std::atoi(attribute(tag, "weight").c_str());
                out[lower(name)] = std::move(alias);
            }
        }
        return out;
    }();
    return families;
}

/// The system's name for `name` if it has such a family, else "".
std::string known_family(const std::string& name) {
    auto& families = system_families();
    auto it = families.find(lower(name));
    return it == families.end() || it->second.fonts.empty() ? std::string() : it->second.name;
}

/// The family's font closest to the weight and style: the style first, then the weight (the first of equals).
bool best_font(const std::string& name, int weight, bool italic, Match& out) {
    auto& families = system_families();
    auto it = families.find(lower(name));
    if (it == families.end() || it->second.fonts.empty()) return false;
    const SystemFamily& family = it->second;
    if (family.weight > 0) weight = family.weight;
    const SystemFont* best = nullptr;
    int bestScore = 0;
    for (const SystemFont& f : family.fonts) {
        int score = std::abs(f.weight - weight) + (f.italic != italic ? 10000 : 0);
        if (!best || score < bestScore) best = &f, bestScore = score;
    }
    out.file = best->file;
    out.index = best->index;
    out.family = family.name;
    out.axes = best->axes;
    return true;
}

/// The system's fallback font for `codepoint` (the NDK's font matcher, as minikin chooses: by script and language);
/// with `emoji`, for it followed by U+FE0F (emoji presentation: the colour emoji font).
bool system_fallback(uint32_t codepoint, bool emoji, int weight, bool italic, const std::string& family, const TextOptions& o, Match& out) {
    uint16_t text[3] = {uint16_t(codepoint), 0, 0};
    uint32_t len = 1;
    if (codepoint >= 0x10000) {
        text[0] = uint16_t(0xD800 + ((codepoint - 0x10000) >> 10));
        text[1] = uint16_t(0xDC00 + ((codepoint - 0x10000) & 0x3FF));
        len = 2;
    }
    if (emoji) text[len++] = 0xFE0F;
    // The NDK's font matcher is API 29: looked up at run time (below it, no fallback here and the run is measured in
    // the list's own font).
    struct Api {
        void* (*create)() = nullptr;
        void (*destroy)(void*) = nullptr;
        void (*style)(void*, uint16_t, bool) = nullptr;
        void (*locales)(void*, const char*) = nullptr;
        void* (*match)(const void*, const char*, const uint16_t*, uint32_t, uint32_t*) = nullptr;
        const char* (*path)(const void*) = nullptr;
        size_t (*index)(const void*) = nullptr;
        size_t (*axes)(const void*) = nullptr;
        uint32_t (*tag)(const void*, uint32_t) = nullptr;
        float (*value)(const void*, uint32_t) = nullptr;
        void (*close)(void*) = nullptr;
        bool ok = false;
    };
    static const Api api = [] {
        Api a;
        void* lib = dlopen("libandroid.so", RTLD_NOW);
        if (!lib) return a;
        a.create = reinterpret_cast<decltype(a.create)>(dlsym(lib, "AFontMatcher_create"));
        a.destroy = reinterpret_cast<decltype(a.destroy)>(dlsym(lib, "AFontMatcher_destroy"));
        a.style = reinterpret_cast<decltype(a.style)>(dlsym(lib, "AFontMatcher_setStyle"));
        a.locales = reinterpret_cast<decltype(a.locales)>(dlsym(lib, "AFontMatcher_setLocales"));
        a.match = reinterpret_cast<decltype(a.match)>(dlsym(lib, "AFontMatcher_match"));
        a.path = reinterpret_cast<decltype(a.path)>(dlsym(lib, "AFont_getFontFilePath"));
        a.index = reinterpret_cast<decltype(a.index)>(dlsym(lib, "AFont_getCollectionIndex"));
        a.axes = reinterpret_cast<decltype(a.axes)>(dlsym(lib, "AFont_getAxisCount"));
        a.tag = reinterpret_cast<decltype(a.tag)>(dlsym(lib, "AFont_getAxisTag"));
        a.value = reinterpret_cast<decltype(a.value)>(dlsym(lib, "AFont_getAxisValue"));
        a.close = reinterpret_cast<decltype(a.close)>(dlsym(lib, "AFont_close"));
        a.ok = a.create && a.destroy && a.style && a.locales && a.match && a.path && a.index && a.axes && a.tag && a.value && a.close;
        return a;
    }();
    if (!api.ok) return false;
    void* matcher = api.create();
    api.style(matcher, uint16_t(std::clamp(weight, 1, 1000)), italic);
    if (!o.locales.empty()) api.locales(matcher, o.locales.c_str());
    uint32_t run = 0;
    // The list's generic family (serif: the serif CJK fonts), as Skia falls back for a TextStyle with that family.
    const std::string generic = o.generic_fallback && (family == "serif" || family == "monospace") ? family : "sans-serif";
    void* font = api.match(matcher, generic.c_str(), text, len, &run);
    api.destroy(matcher);
    if (!font) return false;
    out.file = api.path(font);
    out.index = int(api.index(font));
    out.family = family;
    out.axes.clear();
    for (size_t i = 0, n = api.axes(font); i < n; i++) out.axes.push_back({hb_tag_t(api.tag(font, uint32_t(i))), api.value(font, uint32_t(i))});
    api.close(font);
    // fonts.xml's fallbackFor, which AFontMatcher ignores (observed, API 36: Japanese text in serif matched NotoSansCJK,
    // not the NotoSerifCJK of its family that fonts.xml gives for serif): the stand-in, as Skia (Flutter, the WebView)
    // takes it, when it has the character.
    static const std::vector<FallbackFamily> fallbacks = [] {
        std::ifstream in("/system/etc/fonts.xml");
        std::stringstream ss;
        ss << in.rdbuf();
        return parse_fallback_families(ss.str());
    }();
    FallbackFor sub;
    if (generic != "sans-serif" && fallback_for(fallbacks, out.file, out.index, generic, weight, italic, sub)) {
        Match m;
        m.file = "/system/fonts/" + sub.file;
        m.index = sub.index;
        m.family = family;
        for (const auto& [t, v] : sub.axes) m.axes.push_back({hb_tag_from_string(t.c_str(), 4), v});
        hb_codepoint_t g;
        Face* face = face_for(m, weight, italic);
        if (face && hb_font_get_nominal_glyph(face->font, hb_codepoint_t(codepoint), &g)) out = std::move(m);
    }
    return true;
}

/// The list's generic family: its first generic name (as fonts.xml names them), else sans-serif. Characters its fonts
/// lack fall back through it.
std::string generic_family(const std::vector<std::string>& names) {
    for (const std::string& n : names)
        if (n == "serif" || n == "sans-serif" || n == "monospace") return n;
    return "sans-serif";
}

bool match(const std::vector<std::string>& names, int weight, bool italic, const TextOptions& o, Match& out, int32_t codepoint = -1) {
    if (codepoint < 0) {
        for (const std::string& n : names)
            if (best_font(n, weight, italic, out)) return true;
        return best_font("sans-serif", weight, italic, out);
    }
    // The first font of the list that has the codepoint.
    for (const std::string& n : names) {
        Match m;
        hb_codepoint_t g;
        Face* face = best_font(n, weight, italic, m) ? face_for(m, weight, italic) : nullptr;
        if (face && hb_font_get_nominal_glyph(face->font, hb_codepoint_t(codepoint), &g)) {
            out = std::move(m);
            return true;
        }
    }
    // Else the system's fallback chain (the unnamed families, by script and language).
    return system_fallback(uint32_t(codepoint), false, weight, italic, generic_family(names), o, out);
}
#else
// Linux: fontconfig (the files Flutter's Skia also gets from fontconfig).

std::map<std::string, std::string> g_knownFamily;

/// `name` if fontconfig has a family by that name, else "".
std::string known_family(const std::string& name) {
    auto it = g_knownFamily.find(name);
    if (it != g_knownFamily.end()) return it->second;
    FcPattern* p = FcPatternCreate();
    FcPatternAddString(p, FC_FAMILY, (const FcChar8*)name.c_str());
    FcObjectSet* os = FcObjectSetBuild(FC_FAMILY, nullptr);
    FcFontSet* set = FcFontList(nullptr, p, os);
    bool found = set && set->nfont > 0;
    if (set) FcFontSetDestroy(set);
    FcObjectSetDestroy(os);
    FcPatternDestroy(p);
    return g_knownFamily[name] = found ? name : std::string();
}

bool match(const std::vector<std::string>& names, int weight, bool italic, const TextOptions& o, Match& out, int32_t codepoint = -1) {
    FcPattern* p = FcPatternCreate();
    for (const std::string& n : names) FcPatternAddString(p, FC_FAMILY, (const FcChar8*)n.c_str());
    FcPatternAddDouble(p, FC_WEIGHT, FcWeightFromOpenTypeDouble(std::clamp(weight, 1, 1000)));
    FcPatternAddInteger(p, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    // Generic families (serif, sans-serif) and fallbacks prefer fonts for the language, as Chromium does for lang
    // (Japanese text: "ja").
    if (!o.language.empty()) FcPatternAddString(p, FC_LANG, (const FcChar8*)o.language.c_str());
    FcCharSet* cs = nullptr;
    if (codepoint >= 0) {
        cs = FcCharSetCreate();
        FcCharSetAddChar(cs, FcChar32(codepoint));
        FcPatternAddCharSet(p, FC_CHARSET, cs);
    }
    FcConfigSubstitute(nullptr, p, FcMatchPattern);
    FcDefaultSubstitute(p);
    FcResult r;
    FcPattern* m = FcFontMatch(nullptr, p, &r);
    if (cs) FcCharSetDestroy(cs);
    FcPatternDestroy(p);
    if (!m) return false;
    FcChar8* file = nullptr;
    FcChar8* family = nullptr;
    bool ok = FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch;
    if (ok) {
        out.file = (const char*)file;
        out.index = 0;
        FcPatternGetInteger(m, FC_INDEX, 0, &out.index);
        out.family = FcPatternGetString(m, FC_FAMILY, 0, &family) == FcResultMatch ? (const char*)family : "";
        out.axes.clear();
    }
    FcPatternDestroy(m);
    return ok;
}
#endif

/// The CSS list as system family names: QtBackend::resolve_families' choices, with the generic names (which both
/// fontconfig and Android's fonts.xml know).
std::vector<std::string> resolve_families(const std::string& families) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= families.size()) {
        size_t comma = families.find(',', start);
        std::string family = trim(families.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        start = comma == std::string::npos ? families.size() + 1 : comma + 1;
        if (family.empty()) continue;
        std::string l = lower(family);
        if (l == "-apple-system" || l == "blinkmacsystemfont" || l == "system-ui" || l == "ui-sans-serif" || l == "sans-serif" || l == "roboto" ||
            l == "inter" || l == "helvetica" || l == "helvetica neue") {
            out.push_back("sans-serif");
        } else if (l == "serif" || l == "ui-serif") {
            out.push_back("serif");
        } else if (l == "monospace" || l == "ui-monospace" || l == "menlo" || l == "sf mono") {
            std::string mono = "monospace";
            for (const char* name : {"SF Mono", "Menlo", "Consolas", "Cascadia Mono", "DejaVu Sans Mono"})
                if (std::string known = known_family(name); !known.empty()) {
                    mono = known;
                    break;
                }
            out.push_back(mono);
        } else if (l == "cursive" || l == "fantasy") {
            out.push_back(l);
        } else if (l == "emoji" || l == "apple color emoji") {
            bool any = false;
            for (const char* name : {"Segoe UI Emoji", "Apple Color Emoji", "Noto Color Emoji"})
                if (std::string known = known_family(name); !known.empty()) {
                    out.push_back(known);
                    any = true;
                }
            if (!any) out.push_back("emoji");
        } else if (std::string known = known_family(family); !known.empty()) {
            out.push_back(known);
        }
    }
    if (out.empty()) out.push_back("sans-serif");
    return out;
}

/// A colour emoji font that has `codepoint`, for a cluster in emoji presentation. Android: the system's font for it
/// followed by U+FE0F, through the list's generic family (`names`). Every character the list's fonts lack is asked for
/// this way first (as Skia does), so the generic family's fallbackFor fonts must apply here too (observed: Japanese in
/// serif asked through sans-serif came out in NotoSansCJK whatever the list).
bool match_emoji(uint32_t codepoint, int weight, bool italic, const TextOptions& o, Match& out, const std::vector<std::string>& names) {
#ifdef __ANDROID__
    return system_fallback(codepoint, true, weight, italic, generic_family(names), o, out);
#else
    (void)names;
    return match(resolve_families("emoji"), weight, italic, o, out, int32_t(codepoint));
#endif
}

/// Reads the face's metrics (its tables) and makes its HarfBuzz font, from its file or its bytes; false when neither
/// holds a font.
bool load_face(Face* f, const std::vector<hb_variation_t>& systemAxes, int weight) {
    hb_blob_t* blob = f->bytes ? hb_blob_create((const char*)f->bytes->data(), unsigned(f->bytes->size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr)
                               : hb_blob_create_from_file(f->file.c_str());
    {
        // Metrics from the tables, as Qt picks them: OS/2 typo when USE_TYPO_METRICS, else hhea (else OS/2 win).
        hb_face_t* probe = hb_face_create(blob, unsigned(f->index) & 0xFFFF);
        auto table = [&](const char* tag, std::vector<uint8_t>& out) {
            hb_blob_t* t = hb_face_reference_table(probe, HB_TAG(tag[0], tag[1], tag[2], tag[3]));
            unsigned n = 0;
            const char* d = hb_blob_get_data(t, &n);
            out.assign(d, d + n);
            hb_blob_destroy(t);
            return n > 0;
        };
        auto s16 = [](const std::vector<uint8_t>& b, size_t at) { return at + 2 <= b.size() ? int16_t(b[at] << 8 | b[at + 1]) : int16_t(0); };
        auto u16 = [](const std::vector<uint8_t>& b, size_t at) { return at + 2 <= b.size() ? uint16_t(b[at] << 8 | b[at + 1]) : uint16_t(0); };
        std::vector<uint8_t> os2, hhea, head;
        const bool has_os2 = table("OS/2", os2) && os2.size() >= 78, has_hhea = table("hhea", hhea) && hhea.size() >= 10;
        const bool has_head = table("head", head) && head.size() >= 46;
        if (!has_os2 && !has_hhea && !has_head) {
            hb_face_destroy(probe);
            hb_blob_destroy(blob);
            return false;
        }
        f->upem = hb_face_get_upem(probe) ? float(hb_face_get_upem(probe)) : 1000;
        if (has_os2) {
            const uint16_t version = u16(os2, 0), selection = u16(os2, 62);
            f->hasTypo = true;
            f->typoAscent = s16(os2, 68);
            f->typoDescent = -s16(os2, 70);
            if (version >= 2 && os2.size() >= 88) f->xHeight = s16(os2, 86);
            f->info.weightClass = u16(os2, 4);
            f->info.italic = (selection & 1) != 0;
            if (selection & 128) {
                f->ascent = s16(os2, 68);
                f->descent = -s16(os2, 70);
                f->leading = s16(os2, 72);
            }
        }
        if (f->ascent == 0 && f->descent == 0) {
            if (has_hhea && (s16(hhea, 4) || s16(hhea, 6))) {
                f->ascent = s16(hhea, 4);
                f->descent = -s16(hhea, 6);
                f->leading = s16(hhea, 8);
            } else if (has_os2) {
                f->ascent = u16(os2, 74);
                f->descent = u16(os2, 76);
            }
        }
        if (!has_os2 && has_head) f->info.italic = (u16(head, 44) & 2) != 0;  // macStyle
        hb_face_destroy(probe);
    }
    hb_face_t* face = hb_face_create(blob, unsigned(f->index) & 0xFFFF);
    hb_blob_destroy(blob);
    f->font = hb_font_create(face);
    hb_face_destroy(face);
    if (f->index >> 16) hb_font_set_var_named_instance(f->font, unsigned(f->index >> 16) - 1);
    // A variable font at the CSS weight, as Flutter asks Skia for it (FontVariation 'wght'), over the axes the system's
    // font list sets for the style (Android: Roboto's 'ital', for one).
    std::vector<hb_variation_t> axes;
    for (const hb_variation_t& a : systemAxes)
        if (a.tag != HB_TAG('w', 'g', 'h', 't')) axes.push_back(a);
    axes.push_back({HB_TAG('w', 'g', 'h', 't'), float(weight)});
    hb_font_set_variations(f->font, axes.data(), unsigned(axes.size()));
    {
        // Where every axis ended up (the named instance, the system's list, the weight; clamped to its range).
        hb_face_t* hf = hb_font_get_face(f->font);
        unsigned n = hb_ot_var_get_axis_count(hf);
        std::vector<hb_ot_var_axis_info_t> infos(n);
        hb_ot_var_get_axis_infos(hf, 0, &n, infos.data());
        unsigned nc = 0;
        const float* coords = hb_font_get_var_coords_design(f->font, &nc);
        for (unsigned i = 0; i < n; i++) {
            char tag[5] = {};
            hb_tag_to_string(infos[i].tag, tag);
            f->info.axes.push_back({tag, i < nc && coords ? coords[i] : infos[i].default_value});
        }
    }
    hb_font_set_scale(f->font, int(f->upem), int(f->upem));
    hb_font_make_immutable(f->font);
    if (f->xHeight <= 0) {
        hb_codepoint_t g;
        hb_glyph_extents_t e;
        if (hb_font_get_nominal_glyph(f->font, 'x', &g) && hb_font_get_glyph_extents(f->font, g, &e)) f->xHeight = float(e.y_bearing);
    }
    f->info.file = f->file;
    f->info.index = f->index;
    g_faceById.push_back(f);
    f->info.id = int(g_faceById.size());
    return true;
}

Face* face_for(const Match& m, int weight, bool italic) {
    auto key = std::make_tuple(m.file, m.index, weight, italic);
    auto it = g_faces.find(key);
    if (it != g_faces.end()) return it->second.get();
    auto f = std::make_unique<Face>();
    f->file = m.file;
    f->index = m.index;
    f->family = m.family;
    f->info.family = m.family;
    load_face(f.get(), m.axes, weight);
    Face* raw = f.get();
    g_faces.emplace(key, std::move(f));
    return raw;
}

/// Advance of `cps` in font units of `face`.
float measure_run(Face* face, const std::vector<uint32_t>& cps, size_t from, size_t to, const std::vector<hb_feature_t>& features) {
    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_codepoints(buf, cps.data(), int(cps.size()), unsigned(from), int(to - from));
    hb_buffer_guess_segment_properties(buf);
    hb_shape(face->font, buf, features.data(), unsigned(features.size()));
    unsigned n = 0;
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
    double w = 0;
    for (unsigned i = 0; i < n; i++) w += pos[i].x_advance;
    hb_buffer_destroy(buf);
    return float(w);
}

std::vector<uint32_t> utf8(const std::string& s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        int n;
        if (c < 0x80) cp = c, n = 1;
        else if ((c >> 5) == 6) cp = c & 0x1F, n = 2;
        else if ((c >> 4) == 14) cp = c & 0x0F, n = 3;
        else if ((c >> 3) == 30) cp = c & 0x07, n = 4;
        else { out.push_back(0xFFFD); i++; continue; }
        if (i + size_t(n) > s.size()) { out.push_back(0xFFFD); break; }
        for (int k = 1; k < n; k++) cp = (cp << 6) | ((unsigned char)s[i + size_t(k)] & 0x3F);
        out.push_back(cp);
        i += size_t(n);
    }
    return out;
}

} // namespace

double font_setup_ms() {
    std::lock_guard l(g_mutex);
    return g_setupMs;
}

TextBackend::TextBackend(TextOptions options) : options_(std::move(options)) {
    auto key = std::make_tuple(options_.language, options_.locales, options_.generic_fallback);
    std::lock_guard l(g_mutex);
    auto it = std::find(g_options.begin(), g_options.end(), key);
    optionsId_ = int(it - g_options.begin());
    if (it == g_options.end()) g_options.push_back(key);
}

TextBackend::~TextBackend() {
    for (auto& [id, f] : fonts_) delete f.hbFeatures;
    // The book's faces go with it: nothing may find them any more.
    std::lock_guard l(g_mutex);
    for (auto& face : ownFaces_) {
        g_faceById[size_t(face->info.id - 1)] = nullptr;
        for (auto it = g_glyphFallback.begin(); it != g_glyphFallback.end();)
            it = std::get<3>(it->first) == face.get() || it->second == face.get() ? g_glyphFallback.erase(it) : std::next(it);
        hb_font_destroy(face->font);
    }
}

int TextBackend::create_font(const std::string& families, float size, int weight, bool italic, const std::string& features, twlite::FontMetrics& out) {
    if (!options_.reuse_fonts) return create_font_uncached(families, size, weight, italic, features, out);
    // Asked again (another chapter, part or layout): the same font, its widths kept.
    char numbers[64];
    std::snprintf(numbers, sizeof numbers, "|%g|%d|%d|", double(size), weight, int(italic));
    std::string key = families + numbers + features;
    if (auto known = fontKeys_.find(key); known != fontKeys_.end()) {
        out = known->second.second;
        return known->second.first;
    }
    int made = create_font_uncached(families, size, weight, italic, features, out);
    if (made) fontKeys_[key] = {made, out};
    return made;
}

int TextBackend::create_font_uncached(const std::string& families, float size, int weight, bool italic, const std::string& features, twlite::FontMetrics& out) {
    Face* face = nullptr;
    std::string family;
    std::vector<std::string> fallbacks;
    {
        std::lock_guard l(g_mutex);
        auto t0 = std::chrono::steady_clock::now();
        auto key = std::make_tuple(families, weight, italic, optionsId_);
        auto it = g_resolved.find(key);
        if (it == g_resolved.end()) {
            std::vector<std::string> names = resolve_families(families);
            Resolved r;
            Match m;
            if (match(names, weight, italic, options_, m)) {
                r.face = face_for(m, weight, italic);
                r.family = m.family;
            }
            // The real family behind each name: Skia knows no fontconfig aliases.
            for (const std::string& n : names) {
                Match one;
                if (match({n}, weight, italic, options_, one) && std::find(r.fallbacks.begin(), r.fallbacks.end(), one.family) == r.fallbacks.end())
                    r.fallbacks.push_back(one.family);
            }
            it = g_resolved.emplace(key, std::move(r)).first;
        }
        face = it->second.face;
        family = it->second.family;
        fallbacks = it->second.fallbacks;
        g_setupMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    // The book's fonts the list names come before the system's (CSS Fonts 4 §4.5: @font-face families first).
    std::vector<Face*> book;
    if (!bookFonts_.empty()) {
        size_t start = 0;
        while (start <= families.size()) {
            size_t comma = families.find(',', start);
            std::string name = lower(trim(families.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
            start = comma == std::string::npos ? families.size() + 1 : comma + 1;
            auto found = bookFonts_.find(name);
            if (found == bookFonts_.end()) continue;
            // The style first, then the nearest weight (CSS Fonts 4 §5.2, simplified).
            const BookFont* best = nullptr;
            int bestScore = 0;
            for (const BookFont& b : found->second) {
                int score = std::abs(b.weight - weight) + (b.italic != italic ? 10000 : 0);
                if (!best || score < bestScore) best = &b, bestScore = score;
            }
            if (best && std::find(book.begin(), book.end(), best->face) == book.end()) book.push_back(best->face);
        }
    }
    if (!book.empty()) {
        face = book.front();
        family = face->info.family;
    }
    if (!face) return 0;

    float s = size / face->upem;
    out.ascent = face->ascent * s;
    out.descent = face->descent * s;
    out.height = (face->ascent + face->descent + face->leading) * s;
    out.x_height = face->xHeight * s;
    hb_codepoint_t zero;
    out.zero_width = hb_font_get_nominal_glyph(face->font, '0', &zero) ? hb_font_get_glyph_h_advance(face->font, zero) * s : size / 2;
    if (face->hasTypo) twlite::set_em_box(out, face->typoAscent, face->typoDescent, size);

    int id = ++nextFont_;
    Font& f = fonts_[id];
    f.family = family;
    f.fallbacks = std::move(fallbacks);
    f.size = size;
    f.weight = weight;
    f.italic = italic;
    f.features = features;
    f.face = face;
    f.bookFaces = std::move(book);
    f.hbFeatures = new std::vector<hb_feature_t>();
    size_t start = 0;
    while (start < features.size()) {
        size_t comma = features.find(',', start);
        std::string entry = features.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        start = comma == std::string::npos ? features.size() : comma + 1;
        hb_feature_t feat;
        if (hb_feature_from_string(entry.c_str(), int(entry.size()), &feat)) f.hbFeatures->push_back(feat);
    }
    fontsVersion_++;
    return id;
}

void TextBackend::delete_font(int) {
    // Kept: a display list painted before the delete may still name it, and the Dart painter looks it up.
}

namespace {
#ifndef __ANDROID__
/// Emoji_Presentation=Yes (Unicode 16.0 emoji-data.txt): the characters that are emoji without U+FE0F. The others
/// (⏱, ❤, 🛋, ...) are text unless U+FE0F asks for emoji, as Chromium on Linux draws them.
bool emoji_presentation(uint32_t c) {
    static const uint32_t ranges[][2] = {
    {0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F},
    {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA},
    {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C},
    {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50},
    {0x2B55, 0x2B55}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F1E6, 0x1F1FF}, {0x1F201, 0x1F201}, {0x1F21A, 0x1F21A},
    {0x1F22F, 0x1F22F}, {0x1F232, 0x1F236}, {0x1F238, 0x1F23A}, {0x1F250, 0x1F251}, {0x1F300, 0x1F320}, {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393},
    {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D},
    {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC},
    {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D7}, {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A},
    {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FA7C}, {0x1FA80, 0x1FA89}, {0x1FA8F, 0x1FAC6}, {0x1FACE, 0x1FADC}, {0x1FADF, 0x1FAE9}, {0x1FAF0, 0x1FAF8},
    };
    size_t lo = 0, hi = sizeof ranges / sizeof ranges[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c < ranges[mid][0]) hi = mid;
        else if (c > ranges[mid][1]) lo = mid + 1;
        else return true;
    }
    return false;
}

/// `plain` drawn bold or slanted by the engine (a face of its own that asks for the weight or style its file does not
/// have: the painter asks Flutter for it, which emboldens or skews the glyphs, as Skia does for Chromium; the advances
/// stay the file's).
Face* synthetic(Face* plain, bool bold, bool italic) {
    if (!bold && !italic) return plain;
    static std::map<std::tuple<Face*, bool, bool>, std::unique_ptr<Face>> made;
    auto key = std::make_tuple(plain, bold, italic);
    auto it = made.find(key);
    if (it != made.end()) return it->second.get();
    auto f = std::make_unique<Face>(*plain);
    hb_font_reference(f->font);
    if (bold) f->info.weightClass = 700;
    if (italic) f->info.italic = true;
    Face* raw = f.get();
    g_faceById.push_back(raw);
    raw->info.id = int(g_faceById.size());
    made.emplace(key, std::move(f));
    return raw;
}
#endif

/// What attaches to the character before it: combining marks, variation selectors, emoji modifiers and tags.
bool extends(uint32_t c) {
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF) ||
           (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F) || (c >= 0x1F3FB && c <= 0x1F3FF) || (c >= 0xE0020 && c <= 0xE007F) ||
           (c >= 0xE0100 && c <= 0xE01EF);
}

/// The face each character of `cps` is measured with. A cluster (a character with what extends it, emoji joined by
/// U+200D, a flag's two letters) stays in one face. A character the font's face lacks goes to the colour emoji font
/// when that has it (unless U+FE0E asks for text), as Flutter's own text stack and browsers fall back for emoji, else
/// to the system's fallback for it; an emoji sequence (U+FE0F, a modifier, a joined sequence, a keycap) goes to the
/// colour emoji font even when the face has its first character.
std::vector<Face*> faces_of(const TextBackend::Font& f, const std::vector<uint32_t>& cps, const TextOptions& o, int oid) {
    std::vector<Face*> faceOf(cps.size(), f.face);
    for (size_t i = 0; i < cps.size();) {
        size_t j = i + 1;
        bool text = false, sequence = false;
        bool flag = cps[i] >= 0x1F1E6 && cps[i] <= 0x1F1FF;
        while (j < cps.size()) {
            uint32_t c = cps[j];
            if (extends(c)) {
                if (c == 0xFE0F || c == 0x20E3 || (c >= 0x1F3FB && c <= 0x1F3FF) || (c >= 0xE0020 && c <= 0xE007F)) sequence = true;
                if (c == 0xFE0E) text = true;
                j++;
            } else if (c == 0x200D && j + 1 < cps.size()) {
                sequence = true;
                j += 2;
            } else if (flag && j == i + 1 && c >= 0x1F1E6 && c <= 0x1F1FF) {
                j++;
            } else {
                break;
            }
        }
        uint32_t base = cps[i];
        hb_codepoint_t g;
        Face* face = f.face;
        if (base >= 0x20 && !(text && hb_font_get_nominal_glyph(f.face->font, base, &g))) {
#ifdef __ANDROID__
            bool wantEmoji = !text;  // as Skia's own fallback and Android's Chromium do (android-fidelity report)
#else
            bool wantEmoji = !text && (sequence || emoji_presentation(base));
#endif
            bool primary = hb_font_get_nominal_glyph(f.face->font, base, &g);
            if (!primary)
                for (Face* b : f.bookFaces)
                    if (b != f.face && hb_font_get_nominal_glyph(b->font, base, &g)) {
                        face = b;
                        primary = true;
                        break;
                    }
            if (!primary || (wantEmoji && sequence)) {
                std::lock_guard l(g_mutex);
                auto key = std::make_tuple(base | (wantEmoji ? 0x80000000u : 0), f.weight, f.italic, f.face, oid);
                auto fb = g_glyphFallback.find(key);
                if (fb == g_glyphFallback.end()) {
                    Match m;
                    Face* found = nullptr;
                    if (wantEmoji && match_emoji(base, f.weight, f.italic, o, m, f.fallbacks)) {
                        found = face_for(m, f.weight, f.italic);
                        if (!hb_font_get_nominal_glyph(found->font, base, &g)) found = nullptr;
                    }
#ifdef __ANDROID__
                    if (!found && !primary && match(f.fallbacks, f.weight, f.italic, o, m, int32_t(base))) found = face_for(m, f.weight, f.italic);
#else
                    // As Chromium on Linux (FontCache::PlatformFallbackFontForCharacter): a family of the CSS list that
                    // has the character, at the asked weight and style; else the system's font for it at the normal
                    // weight and style, made bold from 600 and slanted when asked (OBSERVED: ▲ in a 600 <th>, Chromium
                    // NotoSansMono-Regular, we had NotoSansMono-Bold; bold CJK in NotoSansCJK-Regular).
                    if (!found && !primary) {
                        if (match(f.fallbacks, f.weight, f.italic, o, m, int32_t(base)) &&
                            std::find(f.fallbacks.begin(), f.fallbacks.end(), m.family) != f.fallbacks.end()) {
                            found = face_for(m, f.weight, f.italic);
                        } else if (match({}, 400, false, o, m, int32_t(base))) {
                            Face* plain = face_for(m, 400, false);
                            found = synthetic(plain, f.weight >= 600 && plain->info.weightClass < 600, f.italic && !plain->info.italic);
                        }
                        if (found && !hb_font_get_nominal_glyph(found->font, base, &g)) found = nullptr;
                    }
#endif
                    fb = g_glyphFallback.emplace(key, found ? found : f.face).first;
                }
                face = fb->second;
            }
        }
        for (size_t k = i; k < j; k++) faceOf[k] = face;
        i = j;
    }
    return faceOf;
}
} // namespace

float TextBackend::text_width(int font, const std::string& text) {
    auto it = fonts_.find(font);
    if (it == fonts_.end() || text.empty()) return 0;
    Font& f = it->second;
    auto cached = f.widths.find(text);
    if (cached != f.widths.end()) return cached->second;

    std::vector<uint32_t> cps = utf8(text);
    std::vector<Face*> faceOf = faces_of(f, cps, options_, optionsId_);
    double w = 0;
    for (size_t i = 0; i < cps.size();) {
        size_t j = i + 1;
        while (j < cps.size() && faceOf[j] == faceOf[i]) j++;
        w += measure_run(faceOf[i], cps, i, j, *f.hbFeatures) * (f.size / faceOf[i]->upem);
        i = j;
    }
    float width = float(w);
    if (text.size() <= 256) {
        if (f.widths.size() > 4096) f.widths.clear();
        f.widths.emplace(text, width);
    }
    return width;
}

void TextBackend::text_advances(int font, const std::string& text, std::vector<float>& prefix) {
    prefix.assign(text.size() + 1, 0.0f);
    auto it = fonts_.find(font);
    if (it == fonts_.end() || text.empty()) return;
    Font& f = it->second;
    std::vector<uint32_t> cps = utf8(text);
    std::vector<Face*> faceOf = faces_of(f, cps, options_, optionsId_);
    std::vector<double> adv(cps.size(), 0.0);  // per character
    for (size_t i = 0; i < cps.size();) {
        size_t j = i + 1;
        while (j < cps.size() && faceOf[j] == faceOf[i]) j++;
        Face* face = faceOf[i];
        const double k = f.size / face->upem;
        hb_buffer_t* buf = hb_buffer_create();
        // Context on both sides (HarfBuzz's pre/post context), only the characters of this face shaped; the cluster
        // of each glyph is its first character's index in `cps`.
        hb_buffer_add_codepoints(buf, cps.data(), int(cps.size()), unsigned(i), int(j - i));
        hb_buffer_guess_segment_properties(buf);
        hb_shape(face->font, buf, f.hbFeatures->data(), unsigned(f.hbFeatures->size()));
        unsigned n = 0;
        hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
        hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
        for (unsigned g = 0; g < n; g++)
            if (info[g].cluster < cps.size()) adv[info[g].cluster] += pos[g].x_advance * k;
        hb_buffer_destroy(buf);
        i = j;
    }
    // Characters to bytes: each character's advance is counted at its last byte, so prefix[b] at a character boundary
    // is the advance of everything before it.
    size_t c = 0;
    double sum = 0;
    for (size_t b = 0; b < text.size();) {
        const unsigned char lead = (unsigned char)text[b];
        size_t n = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 1;
        if (b + n > text.size()) n = text.size() - b;
        for (size_t x = 0; x < n; x++) prefix[b + x] = float(sum);
        if (c < adv.size()) sum += adv[c];
        c++;
        b += n;
    }
    prefix[text.size()] = float(sum);
}

std::vector<TextBackend::Segment> TextBackend::segments(int font, const std::string& text) {
    std::vector<Segment> out;
    auto it = fonts_.find(font);
    if (it == fonts_.end() || text.empty()) return out;
    std::vector<uint32_t> cps = utf8(text);
    std::vector<Face*> faceOf = faces_of(it->second, cps, options_, optionsId_);
    for (size_t i = 0; i < cps.size(); i++) {
        int units = cps[i] >= 0x10000 ? 2 : 1;
        if (!out.empty() && out.back().face == faceOf[i]) out.back().utf16 += units;
        else out.push_back({faceOf[i], units});
    }
    return out;
}

std::vector<std::pair<float, float>> TextBackend::fallback_extents(int font, const std::string& text) {
    std::vector<std::pair<float, float>> out;
    auto it = fonts_.find(font);
    if (it == fonts_.end() || text.empty()) return out;
    bool ascii = true;
    for (unsigned char c : text) ascii = ascii && c < 0x80;
    if (ascii && it->second.bookFaces.empty()) return out;  // the matched system face has Latin
    std::vector<Face*> seen;
    for (const Segment& seg : segments(font, text)) {
        if (!seg.face || seg.face == it->second.face || std::find(seen.begin(), seen.end(), seg.face) != seen.end()) continue;
        seen.push_back(seg.face);
        const float s = it->second.size / seg.face->upem;
        out.emplace_back(seg.face->ascent * s, seg.face->descent * s);
    }
    return out;
}

bool font_family_available(const std::string& family) {
    const std::string l = lower(trim(family));
    if (l.empty()) return false;
    if (l == "serif" || l == "sans-serif" || l == "monospace" || l == "cursive" || l == "fantasy" || l == "system-ui") return true;
    std::lock_guard lock(g_mutex);
    return !known_family(trim(family)).empty();
}

const FaceInfo& face_info(const Face* face) { return face->info; }
hb_font_t* face_hb_font(const Face* face) { return face->font; }
float face_upem(const Face* face) { return face->upem; }

float face_upem_of(int faceId) {
    std::lock_guard l(g_mutex);
    if (faceId < 1 || size_t(faceId) > g_faceById.size()) return 0;
    const Face* face = g_faceById[size_t(faceId - 1)];
    return face ? face->upem : 0;
}

#if defined(TWT_HB_GPU)
bool glyph_outlines_available() { return true; }

bool encode_glyph_outline(int faceId, uint32_t glyph, GlyphOutline& out) {
    out = {};
    hb_font_t* font = nullptr;
    {
        // A reference of its own: a book's face may go (its backend destroyed) while this one encodes.
        std::lock_guard l(g_mutex);
        if (faceId < 1 || size_t(faceId) > g_faceById.size()) return false;
        const Face* face = g_faceById[size_t(faceId - 1)];
        if (!face || !face->font) return false;
        font = hb_font_reference(face->font);
    }
    // One encoder per thread (it keeps its buffers between glyphs); the face's font is at its units per em, with its
    // variation coordinates, so blobs and extents are in font units.
    struct Draw {
        hb_gpu_draw_t* draw = hb_gpu_draw_create_or_fail();
        ~Draw() { hb_gpu_draw_destroy(draw); }
    };
    thread_local Draw d;
    bool ok = d.draw != nullptr;
    // A colour glyph (COLR layers or paint, a PNG / CBDT / sbix bitmap, an SVG document) is drawn from more than its
    // outline, if it has one at all (an emoji font's glyphs are often empty outlines): none, so that a painter draws
    // its run as before instead of leaving it out.
    hb_face_t* hbFace = hb_font_get_face(font);
    if (ok && (hb_ot_color_glyph_has_paint(hbFace, glyph) || hb_ot_color_glyph_get_layers(hbFace, glyph, 0, nullptr, nullptr) > 0))
        ok = false;
    if (ok && hb_ot_color_has_png(hbFace)) {
        hb_blob_t* png = hb_ot_color_glyph_reference_png(font, glyph);
        if (hb_blob_get_length(png) > 0) ok = false;
        hb_blob_destroy(png);
    }
    if (ok && hb_ot_color_has_svg(hbFace)) {
        hb_blob_t* svg = hb_ot_color_glyph_reference_svg(hbFace, glyph);
        if (hb_blob_get_length(svg) > 0) ok = false;
        hb_blob_destroy(svg);
    }
    if (ok) {
        hb_gpu_draw_reset(d.draw);
        if (hb_gpu_draw_glyph_or_fail(d.draw, font, glyph)) {
            hb_glyph_extents_t e{};
            if (hb_blob_t* blob = hb_gpu_draw_encode(d.draw, &e)) {
                unsigned len = 0;
                const char* data = hb_blob_get_data(blob, &len);
                out.blob.assign(data, data + len);
                out.minX = e.x_bearing;
                out.maxX = e.x_bearing + e.width;
                out.maxY = e.y_bearing;
                out.minY = e.y_bearing + e.height;
                hb_gpu_draw_recycle_blob(d.draw, blob);
            }
        }
    }
    hb_font_destroy(font);
    return ok;
}
#else
bool glyph_outlines_available() { return false; }
bool encode_glyph_outline(int, uint32_t, GlyphOutline& out) {
    out = {};
    return false;
}
#endif

std::vector<uint8_t> face_bytes(int faceId) {
    std::string file;
    int index = 0;
    {
        std::lock_guard l(g_mutex);
        if (faceId < 1 || size_t(faceId) > g_faceById.size()) return {};
        const Face* face = g_faceById[size_t(faceId - 1)];
        if (!face) return {};
        if (face->bytes) return *face->bytes;
        file = face->file;
        index = face->index & 0xFFFF;
    }
    std::vector<uint8_t> data;
    if (FILE* fp = std::fopen(file.c_str(), "rb")) {
        std::fseek(fp, 0, SEEK_END);
        long size = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (size > 0) {
            data.resize(size_t(size));
            if (std::fread(data.data(), 1, data.size(), fp) != data.size()) data.clear();
        }
        std::fclose(fp);
    }
    auto u32 = [&](size_t at) { return at + 4 <= data.size() ? uint32_t(data[at]) << 24 | uint32_t(data[at + 1]) << 16 | uint32_t(data[at + 2]) << 8 | data[at + 3] : 0u; };
    auto u16 = [&](size_t at) { return at + 2 <= data.size() ? uint32_t(data[at]) << 8 | data[at + 1] : 0u; };
    if (data.size() < 12 || std::memcmp(data.data(), "ttcf", 4) != 0) return data;
    // A collection: its member `index` as a font file of its own (the offset table, then each table 4-byte aligned).
    if (uint32_t(index) >= u32(8)) return {};
    size_t dir = u32(12 + 4 * size_t(index));
    uint32_t tables = u16(dir + 4);
    if (dir + 12 + 16 * size_t(tables) > data.size()) return {};
    std::vector<uint8_t> out(data.begin() + long(dir), data.begin() + long(dir + 12 + 16 * tables));
    for (uint32_t t = 0; t < tables; t++) {
        size_t rec = dir + 12 + 16 * t;
        uint32_t offset = u32(rec + 8), length = u32(rec + 12);
        if (size_t(offset) + length > data.size()) return {};
        while (out.size() % 4) out.push_back(0);
        uint32_t at = uint32_t(out.size());
        size_t r = 12 + 16 * t + 8;
        out[r] = uint8_t(at >> 24), out[r + 1] = uint8_t(at >> 16), out[r + 2] = uint8_t(at >> 8), out[r + 3] = uint8_t(at);
        out.insert(out.end(), data.begin() + offset, data.begin() + offset + length);
    }
    return out;
}


std::vector<TextBackend::GlyphRun> TextBackend::shape(int font, const std::string& text) {
    std::vector<GlyphRun> out;
    auto it = fonts_.find(font);
    if (it == fonts_.end() || text.empty()) return out;
    Font& f = it->second;
    std::vector<uint32_t> cps = utf8(text);
    std::vector<Face*> faceOf = faces_of(f, cps, options_, optionsId_);
    double pen = 0;
    for (size_t i = 0; i < cps.size();) {
        size_t j = i + 1;
        while (j < cps.size() && faceOf[j] == faceOf[i]) j++;
        Face* face = faceOf[i];
        float k = f.size / face->upem;
        hb_buffer_t* buf = hb_buffer_create();
        hb_buffer_add_codepoints(buf, cps.data(), int(cps.size()), unsigned(i), int(j - i));
        hb_buffer_guess_segment_properties(buf);
        hb_shape(face->font, buf, f.hbFeatures->data(), unsigned(f.hbFeatures->size()));
        unsigned n = 0;
        hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
        hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
        GlyphRun run;
        run.face = face;
        for (unsigned g = 0; g < n; g++) {
            run.glyphs.push_back(info[g].codepoint);
            run.x.push_back(float(pen + pos[g].x_offset * k));
            run.y.push_back(float(-pos[g].y_offset * k));
            pen += pos[g].x_advance * k;
        }
        hb_buffer_destroy(buf);
        out.push_back(std::move(run));
        i = j;
    }
    return out;
}

bool TextBackend::add_book_font(const std::string& family, int weight, bool italic, std::vector<uint8_t> bytes) {
    if (family.empty() || bytes.size() < 12) return false;
    const uint8_t* b = bytes.data();
    bool sfnt = (b[0] == 0 && b[1] == 1 && b[2] == 0 && b[3] == 0) || std::memcmp(b, "OTTO", 4) == 0 || std::memcmp(b, "true", 4) == 0 ||
                std::memcmp(b, "ttcf", 4) == 0;
    if (!sfnt) return false;  // WOFF and WOFF2 need a decompressor this build does not have
    auto face = std::make_unique<Face>();
    face->bytes = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
    face->family = family;
    face->info.family = family;
    {
        std::lock_guard l(g_mutex);
        if (!load_face(face.get(), {}, weight)) return false;
    }
    bookFonts_[lower(trim(family))].push_back({weight, italic, face.get()});
    ownFaces_.push_back(std::move(face));
    return true;
}

} // namespace twtext
