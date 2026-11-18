// URLs in validated output (SPEC I4, S9.3, S9.4): only `#fragment` references and `data:` images of an allowed type
// (PNG, JPEG, GIF, WebP) within 1024×1024 pixels by their header. Everything else is removed or replaced.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace twcard::urls {

/// The public guide's Limits table: "data: images 1024 × 1024 pixels — a placeholder".
inline constexpr uint32_t kMaxImageSide = 1024;

/// A fixed 1×1 transparent PNG (S9.3): what a too-large or unreadable data image becomes.
extern const char* const kPlaceholderImage;

enum class Kind {
    Fragment,     // `#…`: passes as it is
    Image,        // a data: image that passes, re-encoded as `data:<type>;base64,<bytes>` (`url`)
    Placeholder,  // a data: image of an allowed type that is too large or unreadable: kPlaceholderImage
    Blocked,      // anything else: removed (diagnostic `url-blocked`)
};
struct Verdict {
    Kind kind = Kind::Blocked;
    std::string url;  // what the renderer gets (Fragment, Image, Placeholder)
};

/// A URL from markup (an attribute value) or CSS (a url() argument, escapes already decoded). Leading and trailing
/// ASCII whitespace and C0 controls are ignored, and tab/newline anywhere, as the WHATWG URL parser does.
Verdict check(std::string_view url);

/// A `data:` URL parsed per the WHATWG Fetch "data: URL processor": the MIME type essence (lowercase) and the body.
struct Data {
    std::string mime;
    std::vector<uint8_t> body;
};
std::optional<Data> parse_data_url(std::string_view url);

/// RFC 4648 base64 (with padding).
std::string base64_encode(const uint8_t* data, size_t size);
/// WHATWG Infra "forgiving-base64 decode"; nullopt on failure.
std::optional<std::vector<uint8_t>> forgiving_base64_decode(std::string_view input);

} // namespace twcard::urls
