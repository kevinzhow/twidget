// twidget's gesture policy: SPEC S11.2. Keywords of `touch-action`: W3C Pointer Events, "The touch-action CSS
// property".
#include "twidget/gesture_policy.h"

#include <cmath>
#include <string>

namespace twidget::gesture {

namespace {

bool ascii_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

std::string trimmed_lower(std::string_view v) {
    size_t b = 0, e = v.size();
    while (b < e && ascii_space(v[b])) b++;
    while (e > b && ascii_space(v[e - 1])) e--;
    std::string out(v.substr(b, e - b));
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return out;
}

} // namespace

Claim claim_for_touch_action(std::string_view value) {
    const std::string v = trimmed_lower(value);
    if (v == "none") return {true, false, true};
    if (v == "auto" || v == "manipulation") return {};
    // `[ pan-x | pan-left | pan-right ] || [ pan-y | pan-up | pan-down ] || pinch-zoom`: at most one keyword of each
    // group, whitespace separated. Anything else is invalid (the initial value `auto` applies).
    bool horizontal = false, vertical = false, zoom = false;
    size_t i = 0;
    while (i < v.size()) {
        while (i < v.size() && ascii_space(v[i])) i++;
        size_t j = i;
        while (j < v.size() && !ascii_space(v[j])) j++;
        if (j == i) break;
        std::string_view word = std::string_view(v).substr(i, j - i);
        bool* group = word == "pan-x" || word == "pan-left" || word == "pan-right"  ? &horizontal
                      : word == "pan-y" || word == "pan-up" || word == "pan-down" ? &vertical
                      : word == "pinch-zoom"                                       ? &zoom
                                                                                   : nullptr;
        if (!group || *group) return {};
        *group = true;
        i = j;
    }
    if (!horizontal && !vertical && !zoom) return {};
    return {!horizontal, false, false};
}

Claim claim_for_press(const std::vector<std::string_view>& touch_actions, bool pointerdown_prevented) {
    Claim c;
    for (std::string_view v : touch_actions) {
        Claim one = claim_for_touch_action(v);
        c.horizontal = c.horizontal || one.horizontal;
        c.hold = c.hold || one.hold;
    }
    if (pointerdown_prevented) c.horizontal = true;
    return c;
}

int64_t pack(const Verdict& v) {
    int64_t action = v.action ? int64_t(*v.action) + 1 : 0;
    int64_t event = v.event ? int64_t(*v.event) + 1 : 0;
    return int64_t(v.owner) | int64_t(v.gesture) << 2 | int64_t(v.cancel) << 5 | action << 6 | event << 9 | int64_t(v.edge) << 11;
}

Verdict Arbiter::decide(Press& p, Owner owner, Gesture gesture, std::optional<BubbleAction> action, std::optional<WidgetEvent> event) {
    // `cancel` only in the verdict that hands the press to the bubble.
    bool cancel = owner == Owner::Bubble && p.verdict.owner != Owner::Bubble;
    p.verdict.owner = owner;
    p.verdict.gesture = gesture;
    p.verdict.action = action;
    p.verdict.event = event;
    Verdict v = p.verdict;
    v.cancel = cancel;
    return v;
}

Verdict Arbiter::down(double t, double x, double y, Claim claim, Device device, int button) {
    const bool mouse = device == Device::Mouse;
    Press p{t, x, y, claim, device, false, {}};
    if (!mouse && x <= kEdgeStripPx) {
        p.claim = {};
        p.verdict.edge = true;
    }
    // Every press uses up the last tap: it pairs with it or it does not.
    if (last_tap_) {
        p.second_tap = t - last_tap_->released_at <= kDoubleTapMs && std::hypot(x - last_tap_->x0, y - last_tap_->y0) <= kDoubleTapSlopPx;
        last_tap_.reset();
    }
    press_ = p;
    if (mouse && button == 2) {
        press_->axis_locked = true;  // decided at once (SPEC S11.2): moves change nothing
        if (claim.hold) return decide(*press_, Owner::Widget, Gesture::MenuClick, std::nullopt, WidgetEvent::ContextMenu);
        return decide(*press_, Owner::Bubble, Gesture::MenuClick, BubbleAction::Menu, std::nullopt);
    }
    return press_->verdict;
}

void Arbiter::apply_time(Press& p, double t) {
    if (p.verdict.owner != Owner::Pending || p.device == Device::Mouse || t - p.t0 < kHoldMs) return;
    if (p.claim.hold) decide(p, Owner::Widget, Gesture::Hold, std::nullopt, WidgetEvent::ContextMenu);
    else decide(p, Owner::Bubble, Gesture::Hold, BubbleAction::Menu, std::nullopt);
}

void Arbiter::apply_move(Press& p, double x, double y) {
    if (p.axis_locked) return;
    const double dx = x - p.x0, dy = y - p.y0;
    if (std::hypot(dx, dy) <= kTouchSlopPx) return;
    p.axis_locked = true;
    const bool horizontal = std::fabs(dx) > std::fabs(dy);
    if (p.verdict.owner == Owner::Widget) {
        // A hold the widget won: it keeps it on a horizontal move; a vertical one scrolls the chat.
        if (!horizontal) decide(p, Owner::Bubble, Gesture::DragY, BubbleAction::Scroll, std::nullopt);
        return;
    }
    if (p.verdict.owner != Owner::Pending) return;
    const Gesture drag = horizontal ? Gesture::DragX : Gesture::DragY;
    if (p.device == Device::Mouse || (horizontal && p.claim.horizontal)) {
        decide(p, Owner::Widget, drag, std::nullopt, std::nullopt);
    } else if (horizontal) {
        decide(p, Owner::Bubble, drag, p.verdict.edge ? BubbleAction::Back : BubbleAction::Swipe, std::nullopt);
    } else {
        decide(p, Owner::Bubble, drag, BubbleAction::Scroll, std::nullopt);
    }
}

namespace {
/// The verdict to report after a step: `cancel` set when this step handed the press to the bubble.
Verdict report(const Verdict& before, const Verdict& now) {
    Verdict v = now;
    v.cancel = now.owner == Owner::Bubble && before.owner != Owner::Bubble;
    return v;
}
} // namespace

std::optional<Verdict> Arbiter::move(double t, double x, double y) {
    if (!press_) return std::nullopt;
    Verdict before = press_->verdict;
    apply_time(*press_, t);
    apply_move(*press_, x, y);
    return report(before, press_->verdict);
}

std::optional<Verdict> Arbiter::tick(double t) {
    if (!press_) return std::nullopt;
    Verdict before = press_->verdict;
    apply_time(*press_, t);
    return report(before, press_->verdict);
}

std::optional<Verdict> Arbiter::up(double t, double x, double y) {
    if (!press_) return std::nullopt;
    Press& p = *press_;
    Verdict before = p.verdict;
    apply_time(p, t);
    apply_move(p, x, y);
    if (p.verdict.owner == Owner::Pending) {
        if (p.second_tap) {
            decide(p, Owner::Widget, Gesture::DoubleTap, std::nullopt, WidgetEvent::DblClick);
        } else {
            decide(p, Owner::Widget, Gesture::Tap, std::nullopt, std::nullopt);
            last_tap_ = Tap{t, p.x0, p.y0};
        }
    }
    Verdict v = report(before, p.verdict);
    press_.reset();
    return v;
}

void Arbiter::cancel() {
    press_.reset();
    last_tap_.reset();
}

std::optional<double> Arbiter::holdDeadline() const {
    if (!press_ || press_->device == Device::Mouse || press_->verdict.owner != Owner::Pending) return std::nullopt;
    return press_->t0 + kHoldMs;
}

} // namespace twidget::gesture
