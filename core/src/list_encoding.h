// A display list in the binary display-list encoding of twidget_flutter's frame format v1 (its native/frame_format.h
// documents it, "Display list"): what its Dart painter (DisplayListPainter) reads, for a card's document and layers and
// for a reader's pages. Little-endian (the host's: writer and reader are the same process), 4-byte aligned.
//
// u32 command count, then each command: two header words (u8 kind, u8 flags, u16 0; u32 words of the whole command,
// header included) and its body. With pieces, runs of commands are wrapped in Piece commands (kind 64) carrying the
// digest a painter may cache them by.
#pragma once

#include "display_list.h"

#include <cstdint>
#include <functional>
#include <string>

namespace twlite {

struct ByteWriter {
    std::string out;
    void u32(uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); }
    void u64(uint64_t v) {
        u32(uint32_t(v));
        u32(uint32_t(v >> 32));
    }
    void f32(float v) { out.append(reinterpret_cast<const char*>(&v), 4); }
    void f64(double v) { out.append(reinterpret_cast<const char*>(&v), 8); }
    void pad() {
        while (out.size() % 4) out.push_back(0);
    }
    size_t size() const { return out.size(); }
};

/// The kind of a Piece command (not a Cmd: the encoding's own).
constexpr uint8_t kPieceCommand = 64;

/// `list` encoded. With `pieces`, runs of commands are marked as Piece with their digest; `canvas_generation` gives each
/// canvas's batch count (0 when none), which a canvas image's digest includes.
std::string encode_list(const DisplayList& list, bool pieces = false, const std::function<uint64_t(int canvas)>& canvas_generation = {});

/// Diagnostics: how many pieces and commands in pieces the last encode_list with pieces made (the encoding thread's).
struct PieceStats {
    size_t pieces = 0, commands = 0, outside = 0;
};
PieceStats last_piece_stats();

} // namespace twlite
