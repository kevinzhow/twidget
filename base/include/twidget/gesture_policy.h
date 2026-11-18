// twidget's gesture policy (SPEC S11.2): what a card claims of a press, from `touch-action`, and the arbiter that
// decides, per pointer sequence, whether a press is the card's (widget) or the chat bubble's (scroll, swipe, menu).
// One implementation for every platform: TwidgetCore computes the claim, the hosts' recognizers run the arbiter
// (twidget_flutter through tw_arbiter_*, whose packed encoding the enum values below follow).
//
// All distances are CSS px from the card's leading edge, all times ms on one clock chosen by the caller.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace twidget::gesture {

// SPEC S11.2 "Constants".
inline constexpr double kTouchSlopPx = 10;
inline constexpr double kHoldMs = 500;
inline constexpr double kDoubleTapMs = 300;
inline constexpr double kDoubleTapSlopPx = 40;
inline constexpr double kEdgeStripPx = 20;

/// What the card claims of a press. `vertical` is always false (SPEC S11.2); it is kept so the claim has the shape
/// of the renderer's `gesture` message.
struct Claim {
    bool horizontal = false;  // horizontal drags
    bool vertical = false;
    bool hold = false;        // holds (a long press: the card's `contextmenu`)
    bool operator==(const Claim&) const = default;
};

/// The claim of one `touch-action` value (ASCII-trimmed and lowercased here). Keywords per CSS Pointer Events
/// `touch-action`: `none` claims horizontal drags and holds; a value that allows no horizontal panning (`pan-y`,
/// `pan-up`, `pan-down`, `pinch-zoom`, combinations of them) claims horizontal drags; `auto`, `manipulation` and any
/// value that allows horizontal panning (`pan-x`, `pan-left`, `pan-right`) claim nothing. A value that is not valid
/// `touch-action` syntax claims nothing (the property keeps its initial value `auto`).
Claim claim_for_touch_action(std::string_view value);

/// The claim of a press on an element: the union over the `touch-action` values of the target and its ancestors up
/// to `<html>`, plus horizontal drags (never holds) when the card learned to prevent `pointerdown` there (SPEC S3.3).
Claim claim_for_press(const std::vector<std::string_view>& touch_actions, bool pointerdown_prevented);

enum class Device { Touch = 0, Pen = 1, Mouse = 2 };

enum class Owner { Pending = 0, Widget = 1, Bubble = 2 };
enum class Gesture { Press = 0, Tap = 1, DoubleTap = 2, Hold = 3, DragX = 4, DragY = 5, MenuClick = 6 };
enum class BubbleAction { Menu = 0, Swipe = 1, Scroll = 2, Back = 3 };
enum class WidgetEvent { DblClick = 0, ContextMenu = 1 };

struct Verdict {
    Owner owner = Owner::Pending;
    Gesture gesture = Gesture::Press;
    /// True exactly once per press: in the verdict that hands it to the bubble (the card's pointer sequence is
    /// cancelled then).
    bool cancel = false;
    std::optional<BubbleAction> action;  // what the bubble does, when it owns the press
    std::optional<WidgetEvent> event;    // the card event, present only in the deciding verdict (SPEC S11.2)
    bool edge = false;                   // the press began in the leading-edge strip
    bool operator==(const Verdict&) const = default;
};

/// Packed as tw_arbiter_* returns it (twidget_ffi.h): bits 0-1 owner, 2-4 gesture, bit 5 cancel, 6-8 bubble action
/// (0 none, else value + 1), 9-10 widget event (0 none, else value + 1), bit 11 edge.
int64_t pack(const Verdict& v);

/// One pointer sequence at a time; not thread-safe (one per card per recognizer thread).
class Arbiter {
public:
    /// A press begins. `button`: 0 main, 1 middle, 2 secondary.
    Verdict down(double t, double x, double y, Claim claim, Device device, int button);
    /// The pointer moved; nullopt when no press is open.
    std::optional<Verdict> move(double t, double x, double y);
    /// Time passed without input (a pending touch press may become a hold); nullopt when no press is open.
    std::optional<Verdict> tick(double t);
    /// The press ended (its last position first counts as a move); nullopt when no press is open.
    std::optional<Verdict> up(double t, double x, double y);
    /// Forgets the open press and the last tap.
    void cancel();
    /// When tick() may turn the open press into a hold: its start + kHoldMs, for a pending non-mouse press.
    std::optional<double> holdDeadline() const;

private:
    struct Press {
        double t0, x0, y0;
        Claim claim;
        Device device;
        bool second_tap;
        Verdict verdict;
        bool axis_locked = false;
    };
    struct Tap {
        double released_at, x0, y0;
    };
    Verdict decide(Press& p, Owner owner, Gesture gesture, std::optional<BubbleAction> action, std::optional<WidgetEvent> event);
    void apply_move(Press& p, double x, double y);
    void apply_time(Press& p, double t);

    std::optional<Press> press_;
    std::optional<Tap> last_tap_;
};

} // namespace twidget::gesture
