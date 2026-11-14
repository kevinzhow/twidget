// The binary display-list encoding (list_encoding.h). twidget_flutter's native/frame_format.cpp documents the
// commands' bodies.
#include "list_encoding.h"

#include <cstring>
#include <vector>

namespace twlite {


namespace {

thread_local PieceStats t_stats;

uint32_t argb(const Color& c) { return (uint32_t(c.a) << 24) | (uint32_t(c.r) << 16) | (uint32_t(c.g) << 8) | c.b; }

bool has_rect(Cmd k) { return k != Cmd::ClipPop && k != Cmd::GroupPop && k != Cmd::FilterPop && k != Cmd::Text && k != Cmd::Path; }
bool is_push(Cmd k) { return k == Cmd::ClipPush || k == Cmd::GroupPush || k == Cmd::FilterPush; }
bool is_pop(Cmd k) { return k == Cmd::ClipPop || k == Cmd::GroupPop || k == Cmd::FilterPop; }
Cmd pop_of(Cmd k) { return k == Cmd::ClipPush ? Cmd::ClipPop : k == Cmd::GroupPush ? Cmd::GroupPop : Cmd::FilterPop; }

void stops(ByteWriter& w, const std::vector<Stop>& s) {
    w.u32(uint32_t(s.size()));
    for (const Stop& stop : s) {
        w.f32(stop.offset);
        w.u32(argb(stop.color));
    }
}

void geometry(ByteWriter& w, const std::vector<Path::Op>& ops, const std::vector<float>& points) {
    w.u32(uint32_t(ops.size()));
    w.u32(uint32_t(points.size()));
    for (Path::Op op : ops) w.out.push_back(char(op));
    w.pad();
    for (float v : points) w.f32(v);
}

/// UTF-8 (WTF-8: a lone surrogate's three bytes give that code unit) to UTF-16; a byte that starts no sequence gives
/// U+FFFD.
void utf16(ByteWriter& w, const std::string& s) {
    std::vector<uint16_t> units;
    units.reserve(s.size());
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    size_t n = s.size(), i = 0;
    while (i < n) {
        uint32_t c = p[i], cp = 0xfffd;
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = len > 0 && i + len <= n;
        for (size_t k = 1; ok && k < len; k++) ok = (p[i + k] & 0xc0) == 0x80;
        if (ok) {
            cp = len == 1 ? c : len == 2 ? (c & 0x1f) : len == 3 ? (c & 0x0f) : (c & 0x07);
            for (size_t k = 1; k < len; k++) cp = (cp << 6) | (p[i + k] & 0x3f);
            i += len;
        } else {
            i += 1;
        }
        if (cp >= 0x10000 && cp <= 0x10ffff) {
            cp -= 0x10000;
            units.push_back(uint16_t(0xd800 + (cp >> 10)));
            units.push_back(uint16_t(0xdc00 + (cp & 0x3ff)));
        } else {
            units.push_back(uint16_t(cp > 0x10ffff ? 0xfffd : cp));
        }
    }
    w.u32(uint32_t(units.size()));
    w.out.append(reinterpret_cast<const char*>(units.data()), units.size() * 2);
    w.pad();
}

void svg_path(ByteWriter& w, const Path& p) {
    geometry(w, p.ops, p.points);
    bool fill_gradient = p.fill && p.gradient != 0 && p.stops.size() >= 2;
    bool fill_colour = p.fill && !fill_gradient;
    bool stroke_gradient = p.stroke && p.stroke_gradient != 0 && p.stroke_stops.size() >= 2;
    bool dashes = p.stroke && !p.dashes.empty();
    uint32_t flags = (p.evenodd ? 1 : 0) | (fill_colour ? 2 : 0) | (fill_gradient ? 4 : 0) | (p.stroke ? 8 : 0) | (stroke_gradient ? 16 : 0) |
                     (dashes ? 32 : 0) | (uint32_t(fill_gradient ? p.gradient : 0) & 3) << 8 |
                     (uint32_t(stroke_gradient ? p.stroke_gradient : 0) & 3) << 10 | (uint32_t(p.line_cap) & 3) << 12 |
                     (uint32_t(p.line_join) & 3) << 14;
    w.u32(flags);
    if (fill_colour) w.u32(argb(p.fill_color));
    if (fill_gradient) {
        for (float v : {p.x1, p.y1, p.x2, p.y2, p.r, p.fx, p.fy}) w.f32(v);
        stops(w, p.stops);
    }
    if (p.stroke) {
        w.u32(argb(p.stroke_color));
        w.f32(p.stroke_width);
        w.f32(p.miter_limit);
    }
    if (stroke_gradient) {
        for (float v : p.sg) w.f32(v);
        stops(w, p.stroke_stops);
    }
    if (dashes) {
        w.u32(uint32_t(p.dashes.size()));
        for (float v : p.dashes) w.f32(v);
        w.f32(p.dash_offset);
    }
}

void command(ByteWriter& w, const Command& c) {
    size_t at = w.size();
    uint32_t flags = (c.radii.any() ? 1 : 0) | (c.has_clip ? 2 : 0) | (c.kind == Cmd::GroupPush && c.hidden ? 4 : 0) |
                     (c.kind == Cmd::Shadow && c.inset ? 8 : 0) | (c.kind == Cmd::Image && c.canvas ? 16 : 0) |
                     (c.kind == Cmd::ClipPush && c.path ? 32 : 0) | (c.kind == Cmd::Image ? uint32_t(c.repeat & 3) << 6 : 0);
    w.u32(uint32_t(c.kind) | flags << 8);
    w.u32(0);  // the size, once known
    if (has_rect(c.kind)) {
        w.f32(c.rect.x);
        w.f32(c.rect.y);
        w.f32(c.rect.w);
        w.f32(c.rect.h);
    }
    if (c.radii.any()) {
        const Radii& q = c.radii;
        for (float v : {q.tlx, q.tly, q.trx, q.try_, q.brx, q.bry, q.blx, q.bly}) w.f32(v);
    }
    if (c.has_clip) {
        w.f32(c.clip.x);
        w.f32(c.clip.y);
        w.f32(c.clip.w);
        w.f32(c.clip.h);
    }
    switch (c.kind) {
    case Cmd::FillRect: case Cmd::FillEllipse: w.u32(argb(c.color)); break;
    case Cmd::StrokeRect: case Cmd::StrokeEllipse:
        w.u32(argb(c.color));
        w.f32(c.width);
        break;
    case Cmd::Text:
        w.u32(argb(c.color));
        w.u32(uint32_t(c.font));
        w.f32(c.x);
        w.f32(c.y);
        utf16(w, c.text);
        break;
    case Cmd::Image: w.u32(uint32_t(c.canvas ? c.canvas : c.image)); break;
    case Cmd::Linear: case Cmd::Radial: case Cmd::Conic:
        for (float v : {c.ax, c.ay, c.bx, c.by}) w.f32(v);
        stops(w, c.stops);
        break;
    case Cmd::ClipPush:
        if (c.path) {
            geometry(w, c.path->ops, c.path->points);
            w.u32(c.path->evenodd ? 1 : 0);
        }
        break;
    case Cmd::Path:
        if (c.path) svg_path(w, *c.path);
        else {
            geometry(w, {}, {});
            w.u32(0);
        }
        break;
    case Cmd::GroupPush:
        for (float v : c.matrix) w.f32(v);
        w.f32(c.opacity);
        w.u64(c.key);
        break;
    case Cmd::Shadow:
        w.u32(argb(c.color));
        for (float v : {c.ax, c.ay, c.bx, c.by}) w.f32(v);
        break;
    case Cmd::FilterPush: case Cmd::Backdrop:
        if (!c.filter) {
            w.u32(0);
            break;
        }
        w.u32(uint32_t(c.filter->size()));
        for (const Filter& f : *c.filter) {
            w.u32(uint32_t(f.kind));
            w.f32(f.amount);
            w.f32(f.x);
            w.f32(f.y);
            w.u32(argb(f.color));
        }
        break;
    case Cmd::ClipPop: case Cmd::GroupPop: case Cmd::FilterPop: break;
    }
    uint32_t words = uint32_t((w.size() - at) / 4);
    std::memcpy(&w.out[at + 4], &words, 4);
}

uint64_t fnv(const char* p, size_t n, uint64_t h = 1469598103934665603ull) {
    for (size_t i = 0; i < n; i++) {
        h ^= uint8_t(p[i]);
        h *= 1099511628211ull;
    }
    return h;
}

// Pieces: runs of whole commands and whole small groups, cut where a command's own hash says so (so an insertion
// moves the cuts after it only up to the next cut), and never longer than kMaxPiece.
constexpr size_t kMinPiece = 4, kMaxPiece = 32;

struct PieceEncoder {
    const DisplayList& list;
    ByteWriter& w;
    const std::function<uint64_t(int)>& canvasGen;
    std::vector<int> match;              // a push's pop
    std::vector<int> canvases, backdrops;  // prefix counts
    uint32_t commands = 0;               // emitted, pieces' markers included
    // The open piece.
    bool open = false;
    size_t pieceAt = 0;
    uint32_t pieceCommands = 0;
    uint64_t pieceSalt = 0;

    bool any(const std::vector<int>& prefix, int from, int to) const { return prefix[size_t(to)] - prefix[size_t(from)] > 0; }

    void begin(uint64_t salt = 0) {
        if (open) return;
        open = true;
        pieceAt = w.size();
        pieceCommands = 0;
        pieceSalt = salt;
        w.u32(kPieceCommand);
        w.u32(6);
        w.u64(0);
        w.u32(0);
        w.u32(0);
        commands++;
    }
    void end() {
        if (!open) return;
        open = false;
        size_t body = pieceAt + 24;
        uint64_t digest = fnv(w.out.data() + body, w.size() - body, 1469598103934665603ull ^ pieceSalt);
        uint32_t words = uint32_t((w.size() - body) / 4);
        std::memcpy(&w.out[pieceAt + 8], &digest, 8);
        std::memcpy(&w.out[pieceAt + 16], &pieceCommands, 4);
        std::memcpy(&w.out[pieceAt + 20], &words, 4);
        t_stats.pieces++;
        t_stats.commands += pieceCommands;
    }
    uint64_t one(int i, bool inPiece) {
        size_t at = w.size();
        command(w, list[size_t(i)]);
        commands++;
        if (inPiece) pieceCommands++;
        else t_stats.outside++;
        return fnv(w.out.data() + at, w.size() - at);
    }
    void walk(int from, int to) {
        int i = from;
        while (i < to) {
            const Command& c = list[size_t(i)];
            if (is_push(c.kind)) {
                int e = match[size_t(i)];
                if (!any(canvases, i, e + 1) && !any(backdrops, i, e + 1) && size_t(e - i + 1) <= kMaxPiece) {
                    if (open && pieceCommands + uint32_t(e - i + 1) > kMaxPiece) end();
                    begin();
                    uint64_t h = 0;
                    for (int k = i; k <= e; k++) h = one(k, true);
                    i = e + 1;
                    if (pieceCommands >= kMinPiece && (h & 7) == 0) end();
                } else {
                    end();
                    one(i, false);
                    walk(i + 1, e);
                    one(e, false);
                    i = e + 1;
                }
            } else if (c.kind == Cmd::Image && c.canvas) {
                end();
                begin(canvasGen ? canvasGen(c.canvas) * 0x9e3779b97f4a7c15ull + uint64_t(c.canvas) : 0);
                one(i, true);
                end();
                i++;
            } else if (c.kind == Cmd::Backdrop || is_pop(c.kind)) {
                end();
                one(i, false);
                i++;
            } else {
                begin();
                uint64_t h = one(i, true);
                i++;
                if (pieceCommands >= kMaxPiece || (pieceCommands >= kMinPiece && (h & 7) == 0)) end();
            }
        }
        end();
    }
};

} // namespace

PieceStats last_piece_stats() { return t_stats; }

std::string encode_list(const DisplayList& list, bool pieces, const std::function<uint64_t(int canvas)>& canvas_generation) {
    ByteWriter w;
    w.out.reserve(list.size() * 48 + 8);
    w.u32(0);  // the command count, once known
    t_stats = {};
    // Pieces need pushes and pops that balance, kind by kind.
    std::vector<int> match;
    if (pieces) {
        match.assign(list.size(), -1);
        std::vector<int> stack;
        for (size_t i = 0; i < list.size() && pieces; i++) {
            Cmd k = list[i].kind;
            if (is_push(k)) stack.push_back(int(i));
            else if (is_pop(k)) {
                if (stack.empty() || pop_of(list[size_t(stack.back())].kind) != k) pieces = false;
                else {
                    match[size_t(stack.back())] = int(i);
                    stack.pop_back();
                }
            }
        }
        if (!stack.empty()) pieces = false;
    }
    uint32_t count = 0;
    if (pieces) {
        PieceEncoder e{list, w, canvas_generation, std::move(match), {}, {}};
        e.canvases.assign(list.size() + 1, 0);
        e.backdrops.assign(list.size() + 1, 0);
        for (size_t i = 0; i < list.size(); i++) {
            e.canvases[i + 1] = e.canvases[i] + (list[i].kind == Cmd::Image && list[i].canvas ? 1 : 0);
            e.backdrops[i + 1] = e.backdrops[i] + (list[i].kind == Cmd::Backdrop ? 1 : 0);
        }
        e.walk(0, int(list.size()));
        count = e.commands;
    } else {
        for (const Command& c : list) command(w, c);
        count = uint32_t(list.size());
        t_stats.outside = list.size();
    }
    std::memcpy(&w.out[0], &count, 4);
    return std::move(w.out);
}

} // namespace twlite
