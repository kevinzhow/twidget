// twidget's gesture policy (base/include/twidget/gesture_policy.h) against SPEC S11.2: the claim from touch-action,
// then a table of scripted pointer sequences and the verdict each step must give. Sequences and values are ours.
#include "twidget/gesture_policy.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace twidget::gesture;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) failures++;
}

std::string str(const std::optional<Verdict>& v) {
    if (!v) return "none";
    static const char* owners[] = {"pending", "widget", "bubble"};
    static const char* gestures[] = {"press", "tap", "double-tap", "hold", "drag-x", "drag-y", "menu-click"};
    static const char* actions[] = {"menu", "swipe", "scroll", "back"};
    static const char* events[] = {"dblclick", "contextmenu"};
    std::string s = std::string(owners[int(v->owner)]) + "/" + gestures[int(v->gesture)];
    if (v->cancel) s += " cancel";
    if (v->action) s += std::string(" action=") + actions[int(*v->action)];
    if (v->event) s += std::string(" event=") + events[int(*v->event)];
    if (v->edge) s += " edge";
    return s;
}

const Claim kNone{}, kX{true, false, false}, kXHold{true, false, true};

// One step of a script: the call and the verdict it must give ("none": no open press).
struct Step {
    char op;  // d down, m move, t tick, u up, c cancel
    double t, x, y;
    std::string want;
};

void run(const std::string& name, Claim claim, Device device, int button, const std::vector<Step>& steps) {
    Arbiter a;
    std::string trace;
    bool ok = true;
    for (const Step& s : steps) {
        std::optional<Verdict> v;
        switch (s.op) {
        case 'd': v = a.down(s.t, s.x, s.y, claim, device, button); break;
        case 'm': v = a.move(s.t, s.x, s.y); break;
        case 't': v = a.tick(s.t); break;
        case 'u': v = a.up(s.t, s.x, s.y); break;
        case 'c': a.cancel(); continue;
        }
        std::string got = str(v);
        if (got != s.want) {
            ok = false;
            trace += "\n    at " + std::string(1, s.op) + " t=" + std::to_string(int(s.t)) + ": got " + got + ", want " + s.want;
        }
    }
    check(ok, name + trace);
}

} // namespace

int main() {
    // ── claim from touch-action ──
    check(claim_for_touch_action("none") == kXHold, "none: horizontal drags and holds");
    check(claim_for_touch_action("  NONE ") == kXHold, "trimmed and lowercased");
    check(claim_for_touch_action("pan-y") == kX, "pan-y: horizontal drags");
    check(claim_for_touch_action("pan-up") == kX && claim_for_touch_action("pan-down") == kX, "pan-up, pan-down");
    check(claim_for_touch_action("pinch-zoom") == kX, "pinch-zoom alone");
    check(claim_for_touch_action("pan-y pinch-zoom") == kX, "pan-y pinch-zoom");
    check(claim_for_touch_action("auto") == kNone && claim_for_touch_action("manipulation") == kNone, "auto, manipulation: nothing");
    check(claim_for_touch_action("pan-x") == kNone && claim_for_touch_action("pan-left pan-y") == kNone &&
              claim_for_touch_action("pan-right") == kNone, "horizontal panning allowed: nothing");
    check(claim_for_touch_action("") == kNone && claim_for_touch_action("sideways") == kNone && claim_for_touch_action("none pan-y") == kNone,
          "invalid values: nothing");
    check(claim_for_press({"auto", "pan-y", "auto"}, false) == kX, "an ancestor's pan-y claims horizontal drags");
    check(claim_for_press({"auto", "auto", "none"}, false) == kXHold, "an ancestor's none claims holds too");
    check(claim_for_press({"auto"}, true) == kX, "a learned pointerdown prevention claims horizontal drags, never holds");
    check(claim_for_press({}, false) == kNone, "no values: nothing");

    // ── constants (tw_gesture_constant) ──
    check(kTouchSlopPx == 10 && kHoldMs == 500 && kDoubleTapMs == 300 && kDoubleTapSlopPx == 40 && kEdgeStripPx == 20, "constants");

    // ── the arbiter ──
    run("tap", kNone, Device::Touch, 0, {{'d', 0, 100, 50, "pending/press"}, {'m', 50, 104, 53, "pending/press"}, {'u', 100, 104, 53, "widget/tap"}, {'m', 120, 0, 0, "none"}});
    run("vertical drag goes to the bubble's scroll", kX, Device::Touch, 0,
        {{'d', 0, 100, 50, "pending/press"}, {'m', 10, 102, 70, "bubble/drag-y cancel action=scroll"}, {'m', 20, 102, 90, "bubble/drag-y action=scroll"}, {'u', 30, 102, 95, "bubble/drag-y action=scroll"}});
    run("claimed horizontal drag is the widget's", kX, Device::Touch, 0,
        {{'d', 0, 100, 50, "pending/press"}, {'m', 10, 130, 55, "widget/drag-x"}, {'m', 20, 100, 90, "widget/drag-x"}, {'u', 30, 160, 55, "widget/drag-x"}});
    run("unclaimed horizontal drag is the bubble's swipe", kNone, Device::Touch, 0,
        {{'d', 0, 100, 50, "pending/press"}, {'m', 10, 130, 55, "bubble/drag-x cancel action=swipe"}, {'u', 30, 160, 55, "bubble/drag-x action=swipe"}});
    run("edge strip: no claim, back", kXHold, Device::Touch, 0,
        {{'d', 0, 10, 50, "pending/press edge"}, {'m', 10, 60, 52, "bubble/drag-x cancel action=back edge"}, {'u', 20, 80, 52, "bubble/drag-x action=back edge"}});
    run("edge strip: vertical is scroll", kXHold, Device::Touch, 0,
        {{'d', 0, 19, 50, "pending/press edge"}, {'m', 10, 20, 90, "bubble/drag-y cancel action=scroll edge"}});
    run("exactly 20 px is outside the edge strip", kXHold, Device::Touch, 0,
        {{'d', 0, 20, 50, "pending/press"}, {'m', 10, 60, 50, "widget/drag-x"}});
    run("edge strip: a hold is the bubble's menu (no claim)", kXHold, Device::Pen, 0,
        {{'d', 0, 5, 50, "pending/press edge"}, {'t', 600, 0, 0, "bubble/hold cancel action=menu edge"}});
    run("edge strip does not apply to a mouse", kX, Device::Mouse, 0,
        {{'d', 0, 5, 50, "pending/press"}, {'m', 10, 60, 50, "widget/drag-x"}});
    run("movement within the slop changes nothing", kNone, Device::Touch, 0,
        {{'d', 0, 100, 50, "pending/press"}, {'m', 10, 107, 57, "pending/press"}, {'m', 20, 100, 41, "pending/press"}, {'u', 30, 100, 41, "widget/tap"}});
    run("claimed hold: contextmenu", kXHold, Device::Touch, 0,
        {{'d', 0, 100, 50, "pending/press"}, {'t', 499, 0, 0, "pending/press"}, {'t', 500, 0, 0, "widget/hold event=contextmenu"},
         {'t', 900, 0, 0, "widget/hold"}, {'u', 1000, 100, 50, "widget/hold"}});
    run("unclaimed hold: the bubble's menu", kX, Device::Touch, 0,
        {{'d', 0, 100, 50, "pending/press"}, {'t', 520, 0, 0, "bubble/hold cancel action=menu"}, {'u', 700, 100, 50, "bubble/hold action=menu"}});
    run("a move decides a due hold first", kXHold, Device::Touch, 0,
        {{'d', 0, 100, 50, "pending/press"}, {'m', 600, 101, 51, "widget/hold event=contextmenu"}});
    run("a won hold stays the widget's on a horizontal move", kXHold, Device::Touch, 0,
        {{'d', 0, 100, 50, "pending/press"}, {'t', 500, 0, 0, "widget/hold event=contextmenu"}, {'m', 600, 140, 52, "widget/hold"}});
    run("a won hold becomes the bubble's scroll on a vertical move", kXHold, Device::Touch, 0,
        {{'d', 0, 100, 50, "pending/press"}, {'t', 500, 0, 0, "widget/hold event=contextmenu"}, {'m', 600, 101, 90, "bubble/drag-y cancel action=scroll"},
         {'m', 610, 101, 99, "bubble/drag-y action=scroll"}});
    run("a mouse never holds", kXHold, Device::Mouse, 0,
        {{'d', 0, 100, 50, "pending/press"}, {'t', 2000, 0, 0, "pending/press"}, {'u', 2100, 100, 50, "widget/tap"}});
    run("a mouse drag is the widget's either way", kNone, Device::Mouse, 0,
        {{'d', 0, 100, 50, "pending/press"}, {'m', 10, 102, 90, "widget/drag-y"}, {'u', 20, 102, 99, "widget/drag-y"}});
    run("mouse secondary button with a hold claim: the widget's menu click", kXHold, Device::Mouse, 2,
        {{'d', 0, 100, 50, "widget/menu-click event=contextmenu"}, {'m', 10, 150, 50, "widget/menu-click"}, {'u', 20, 150, 50, "widget/menu-click"}});
    run("mouse secondary button without: the bubble's menu", kX, Device::Mouse, 2,
        {{'d', 0, 100, 50, "bubble/menu-click cancel action=menu"}, {'u', 20, 100, 50, "bubble/menu-click action=menu"}});
    run("up applies the final move", kNone, Device::Touch, 0,
        {{'d', 0, 100, 50, "pending/press"}, {'u', 30, 100, 80, "bubble/drag-y cancel action=scroll"}});

    // Double taps.
    {
        Arbiter a;
        a.down(0, 100, 50, kNone, Device::Touch, 0);
        bool ok = str(a.up(80, 100, 50)) == "widget/tap";
        ok = ok && str(a.down(300, 130, 70, kNone, Device::Touch, 0)) == "pending/press";
        ok = ok && str(a.up(350, 130, 70)) == "widget/double-tap event=dblclick";
        // A third tap right after is a tap again: the double tap used the last tap.
        a.down(400, 130, 70, kNone, Device::Touch, 0);
        ok = ok && str(a.up(420, 130, 70)) == "widget/tap";
        check(ok, "double tap: <= 300 ms after the release, <= 40 px from the first tap's start");
    }
    {
        Arbiter a;
        a.down(0, 100, 50, kNone, Device::Touch, 0);
        a.up(80, 100, 50);
        a.down(381, 100, 50, kNone, Device::Touch, 0);
        check(str(a.up(400, 100, 50)) == "widget/tap", "too late for a double tap");
    }
    {
        Arbiter a;
        a.down(0, 100, 50, kNone, Device::Touch, 0);
        a.up(80, 100, 50);
        a.down(200, 141, 50, kNone, Device::Touch, 0);
        check(str(a.up(220, 141, 50)) == "widget/tap", "too far for a double tap");
    }
    {
        Arbiter a;
        a.down(0, 100, 50, kNone, Device::Touch, 0);
        a.up(80, 100, 50);
        a.cancel();
        a.down(200, 100, 50, kNone, Device::Touch, 0);
        check(str(a.up(220, 100, 50)) == "widget/tap", "cancel forgets the last tap");
    }
    {
        Arbiter a;
        a.down(0, 100, 50, kNone, Device::Touch, 0);
        a.up(80, 100, 50);
        a.down(100, 100, 50, kNone, Device::Touch, 0);
        bool ok = str(a.move(110, 100, 90)) == "bubble/drag-y cancel action=scroll";
        a.up(120, 100, 90);
        a.down(200, 100, 50, kNone, Device::Touch, 0);
        check(ok && str(a.up(210, 100, 50)) == "widget/tap", "a scroll in between is not a tap to pair with");
    }

    // holdDeadline.
    {
        Arbiter a;
        bool ok = !a.holdDeadline();
        a.down(1000, 100, 50, kNone, Device::Touch, 0);
        ok = ok && a.holdDeadline() && *a.holdDeadline() == 1500;
        a.move(1010, 100, 90);
        ok = ok && !a.holdDeadline();
        a.down(2000, 100, 50, kNone, Device::Mouse, 0);
        ok = ok && !a.holdDeadline();
        check(ok, "holdDeadline: start + 500 for a pending non-mouse press only");
    }

    // pack() follows twidget_ffi.h.
    {
        Verdict v;
        v.owner = Owner::Bubble;
        v.gesture = Gesture::DragY;
        v.cancel = true;
        v.action = BubbleAction::Scroll;
        v.edge = true;
        int64_t want = 2 | 5 << 2 | 1 << 5 | 3 << 6 | 0 << 9 | 1 << 11;
        Verdict w;
        w.owner = Owner::Widget;
        w.gesture = Gesture::Hold;
        w.event = WidgetEvent::ContextMenu;
        check(pack(v) == want && pack(w) == (1 | 3 << 2 | 2 << 9), "pack");
    }

    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
