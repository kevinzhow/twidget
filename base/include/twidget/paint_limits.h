// The paint limit (SPEC S10.4, S11.3): how many heavy elements a card may show at once. TwidgetCore counts them
// (core/src/paint_policy.h) and reports `{type:"limit", code, count}`; the card host stops a strict card past either
// limit with the reason below. Values from the public guide's Limits table, as SPEC S10.4 gives them.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace twidget {

/// More than this many heavy boxes at once: `paint-limit`.
inline constexpr int kMaxHeavyBoxes = 16;
/// More than this many boxes with `backdrop-filter` at once: `backdrop-limit`.
inline constexpr int kMaxBackdropBoxes = 2;

enum class PaintLimit { Paint, Backdrop };

/// The `code` of the renderer's `limit` message.
inline constexpr std::string_view paint_limit_code(PaintLimit l) { return l == PaintLimit::Paint ? "paint-limit" : "backdrop-limit"; }

/// The code back; nullopt for an unknown one (the host reports `renderer-violation`).
inline std::optional<PaintLimit> paint_limit_from_code(std::string_view code) {
    if (code == "paint-limit") return PaintLimit::Paint;
    if (code == "backdrop-limit") return PaintLimit::Backdrop;
    return std::nullopt;
}

/// The reason a card stops with: `heavy-elements: N (max 16)` / `backdrop-filter: N (max 2)`.
inline std::string paint_limit_reason(PaintLimit l, int count) {
    return l == PaintLimit::Paint ? "heavy-elements: " + std::to_string(count) + " (max " + std::to_string(kMaxHeavyBoxes) + ")"
                                  : "backdrop-filter: " + std::to_string(count) + " (max " + std::to_string(kMaxBackdropBoxes) + ")";
}

} // namespace twidget
