// SPEC S9.3, S9.4. The data: URL processor follows WHATWG Fetch §"data: URLs"; base64 follows WHATWG Infra
// "forgiving-base64" and RFC 4648. These are written here because no library of PLAN §1 does them, and they only
// ever read a URL's prefix and a small image header's worth of meaning.
#include "urls.h"

#include "png.h"  // twlite::image_dimensions: the image header reader of twidget::display_list
#include "profile.h"

#include <ada.h>
#include <algorithm>

namespace twcard::urls {

const char* const kPlaceholderImage =
    "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR42mNgAAIAAAUAAen63NgAAAAASUVORK5CYII=";

namespace {

bool ascii_whitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return out;
}

/// WHATWG URL: leading/trailing C0 control or space removed, ASCII tab or newline removed anywhere.
std::string url_input(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= 0x20) b++;
    while (e > b && (unsigned char)s[e - 1] <= 0x20) e--;
    std::string out;
    out.reserve(e - b);
    for (size_t i = b; i < e; i++)
        if (s[i] != '\t' && s[i] != '\n' && s[i] != '\r') out += s[i];
    return out;
}

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// URL takes Web IDL USVString inputs. JSON preserves lone surrogates as WTF-8 before this conversion.
std::string usvString(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (size_t offset = 0; offset < value.size(); ++offset) {
        const auto byte = static_cast<unsigned char>(value[offset]);
        if (byte == 0xed && offset + 2 < value.size() &&
            static_cast<unsigned char>(value[offset + 1]) >= 0xa0) {
            result += "\xEF\xBF\xBD";
            offset += 2;
        } else {
            result += value[offset];
        }
    }
    return result;
}

bool allowedLinkSpelling(std::string_view input) {
    constexpr std::string_view punctuation = "-._~:/?#[]@!$&'()*+,;=";
    for (size_t offset = 0; offset < input.size(); ++offset) {
        const auto byte = static_cast<unsigned char>(input[offset]);
        if (byte == 0xed && offset + 2 < input.size() &&
            static_cast<unsigned char>(input[offset + 1]) >= 0xa0)
            return false;
        if (byte >= 0x80 || (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || punctuation.find(char(byte)) != std::string_view::npos)
            continue;
        if (byte != '%' || offset + 2 >= input.size() || hex(input[offset + 1]) < 0 || hex(input[offset + 2]) < 0)
            return false;
        offset += 2;
    }
    return true;
}

std::vector<uint8_t> percent_decode(std::string_view s) {
    std::vector<uint8_t> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            out.push_back(uint8_t(hex(s[i + 1]) * 16 + hex(s[i + 2])));
            i += 2;
        } else {
            out.push_back(uint8_t(s[i]));
        }
    }
    return out;
}

int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

} // namespace

std::string base64_encode(const uint8_t* d, size_t n) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = uint32_t(d[i]) << 16 | uint32_t(d[i + 1]) << 8 | d[i + 2];
        out += alphabet[v >> 18];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = uint32_t(d[i]) << 16;
        out += alphabet[v >> 18];
        out += alphabet[(v >> 12) & 63];
        out += "==";
    } else if (n - i == 2) {
        uint32_t v = uint32_t(d[i]) << 16 | uint32_t(d[i + 1]) << 8;
        out += alphabet[v >> 18];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

std::optional<std::vector<uint8_t>> forgiving_base64_decode(std::string_view input) {
    std::string data;
    data.reserve(input.size());
    for (char c : input)
        if (!ascii_whitespace(c)) data += c;
    if (data.size() % 4 == 0) {
        if (data.size() >= 1 && data.back() == '=') data.pop_back();
        if (data.size() >= 1 && data.back() == '=') data.pop_back();
    }
    if (data.size() % 4 == 1) return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(data.size() * 3 / 4);
    uint32_t buffer = 0;
    int bits = 0;
    for (char c : data) {
        int v = b64_value(c);
        if (v < 0) return std::nullopt;
        buffer = buffer << 6 | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t(buffer >> bits));
        }
    }
    return out;
}

std::optional<Data> parse_data_url(std::string_view raw) {
    std::string url = url_input(raw);
    if (lower(std::string_view(url).substr(0, 5)) != "data:") return std::nullopt;
    std::string_view rest = std::string_view(url).substr(5);
    size_t comma = rest.find(',');
    if (comma == std::string_view::npos) return std::nullopt;
    std::string_view mime = rest.substr(0, comma);
    std::string_view body = rest.substr(comma + 1);
    // Strip leading and trailing ASCII whitespace from the MIME type; a `;base64` parameter (any case, whitespace
    // before it allowed) ends it.
    while (!mime.empty() && ascii_whitespace(mime.front())) mime.remove_prefix(1);
    while (!mime.empty() && ascii_whitespace(mime.back())) mime.remove_suffix(1);
    bool base64 = false;
    {
        std::string lm = lower(mime);
        size_t semi = lm.rfind(';');
        if (semi != std::string::npos) {
            std::string_view param = std::string_view(lm).substr(semi + 1);
            while (!param.empty() && ascii_whitespace(param.front())) param.remove_prefix(1);
            if (param == "base64") {
                base64 = true;
                mime = mime.substr(0, semi);
            }
        }
    }
    Data d;
    std::string essence = lower(mime.substr(0, mime.find(';')));
    while (!essence.empty() && ascii_whitespace(essence.back())) essence.pop_back();
    d.mime = essence.empty() ? "text/plain" : essence;
    std::vector<uint8_t> bytes = percent_decode(body);
    if (base64) {
        auto decoded = forgiving_base64_decode(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        if (!decoded) return std::nullopt;
        d.body = std::move(*decoded);
    } else {
        d.body = std::move(bytes);
    }
    return d;
}

Verdict check(std::string_view raw) {
    std::string url = url_input(raw);
    if (!url.empty() && url[0] == '#') return {Kind::Fragment, url};
    if (lower(std::string_view(url).substr(0, 5)) != "data:") return {Kind::Blocked, {}};
    auto d = parse_data_url(url);
    if (!d) {
        // A data: URL whose type we cannot read cannot be shown either; an image one becomes the placeholder.
        std::string lm = lower(url);
        if (lm.rfind("data:image/", 0) == 0) return {Kind::Placeholder, kPlaceholderImage};
        return {Kind::Blocked, {}};
    }
    if (!profile::image_mime_allowed(d->mime)) return {Kind::Blocked, {}};
    uint32_t w = 0, h = 0;
    if (!twlite::image_dimensions(d->body.data(), d->body.size(), w, h) || w == 0 || h == 0 || w > kMaxImageSide || h > kMaxImageSide)
        return {Kind::Placeholder, kPlaceholderImage};
    return {Kind::Image, "data:" + d->mime + ";base64," + base64_encode(d->body.data(), d->body.size())};
}

std::optional<std::string> css_string(std::string_view text) {
    const auto input = url_input(text);
    if (input.starts_with("//")) return std::nullopt;
    const auto schemeEnd = input.find(':');
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    const auto schemeChar = [&](char c) {
        return letter(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
    };
    // Recognize a WHATWG scheme even when the rest of the URL is malformed.
    if (schemeEnd == std::string::npos || schemeEnd == 0 || !letter(input.front()) ||
        !std::all_of(input.begin() + 1, input.begin() + schemeEnd, schemeChar))
        return std::string(text);
    auto url = check(input);
    if (url.kind == Kind::Blocked) return std::nullopt;
    return std::move(url.url);
}

std::optional<twidget::Json> parse(std::string_view input, std::optional<std::string_view> base) {
    const auto parseWithBase = [&] {
        const auto text = usvString(input);
        if (!base) return ada::parse(text);
        auto parent = ada::parse(usvString(*base));
        return parent ? ada::parse(text, &*parent) : std::move(parent);
    };
    auto url = parseWithBase();
    if (!url) return std::nullopt;
    return twidget::Json::object().set("href", url->get_href()).set("origin", url->get_origin())
        .set("protocol", url->get_protocol()).set("username", url->get_username())
        .set("password", url->get_password()).set("host", url->get_host()).set("hostname", url->get_hostname())
        .set("port", url->get_port()).set("pathname", url->get_pathname())
        .set("search", url->get_search()).set("hash", url->get_hash());
}

std::optional<Navigation> navigation(std::string_view input, Link kind) {
    if (lower(input.substr(0, 8)) != "https://" || !allowedLinkSpelling(input)) return std::nullopt;
    auto url = ada::parse(input);
    if (!url || url->has_credentials() || url->has_empty_hostname()) return std::nullopt;
    auto host = url->get_hostname();
    if (host.ends_with('.')) host.remove_suffix(1);
    const bool telegram = host == "t.me" || host == "telegram.me" || host == "telegram.dog";
    const Link actual = telegram ? Link::Telegram : Link::External;
    if ((kind != Link::Follow && kind != actual) || (!telegram && url->get_href().size() > 256))
        return std::nullopt;
    return Navigation{actual, std::string(url->get_href())};
}

} // namespace twcard::urls
