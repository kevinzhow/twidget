// Heavy boxes (core/src/paint_policy.h) against SPEC S10.4: what makes a box heavy, how animations count, and the
// counter's verdicts at twidget::kMaxHeavyBoxes / kMaxBackdropBoxes. All cases are ours.
#include "paint_policy.h"

#include <cstdio>
#include <string>

using namespace twlite::paint;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) failures++;
}

bool heavy(Style s, bool pseudo = false) {
    auto p = box_paint(s, pseudo);
    return p && p->heavy;
}

} // namespace

int main() {
    // What makes a box heavy (SPEC S10.4: blurred shadow, filter, backdrop-filter, opacity < 1).
    check(!heavy({}), "a plain box is light");
    check(heavy({.box_shadow = "0 2px 6px rgba(0,0,0,.3)"}), "a blurred shadow");
    check(!heavy({.box_shadow = "2px 2px 0 #000"}) && !heavy({.box_shadow = "inset 0 0 0 1px red"}), "a shadow without blur");
    check(heavy({.box_shadow = "1px 1px #000, 0 0 calc(2px + 1px) blue"}), "a blur of unknown size counts");
    check(heavy({.filter = "blur(2px)"}) && heavy({.filter = "brightness(1)"}), "any filter, even one that changes nothing");
    check(!heavy({.filter = "none"}), "filter: none");
    check(heavy({.opacity = "0.5"}) && heavy({.opacity = "0.999"}) && heavy({.opacity = "50%"}), "opacity below 1");
    check(!heavy({.opacity = "1"}) && !heavy({.opacity = "100%"}), "opacity 1");
    {
        auto p = box_paint({.backdrop_filter = "blur(8px)"}, false);
        check(p && p->heavy && p->backdrop, "backdrop-filter: heavy and counted as a backdrop");
    }
    check(!heavy({.opacity = "0.4", .button = Style::Button::Disabled}) && !heavy({.opacity = "0.7", .button = Style::Button::Pressed}),
          "a button's own dimming is not heavy");
    check(heavy({.filter = "blur(1px)", .button = Style::Button::Disabled}), "a dimmed button with a filter is");
    check(!box_paint({.display = "none", .filter = "blur(1px)"}, false), "display: none builds no box");
    check(!box_paint({.display = "contents", .opacity = "0.5"}, false), "display: contents builds no box");
    check(!box_paint({.content = "none", .opacity = "0.5"}, true) && heavy({.content = "\"x\"", .opacity = "0.5"}, true),
          "a pseudo-element is a box only with content");
    check(hides_subtree("none") && hides_subtree(" NONE ") && !hides_subtree("contents"), "hides_subtree");
    check(box_shadow_without_blur("0 0 4px red, 1px 1px 0 blue") == "1px 1px 0 blue" && box_shadow_without_blur("0 0 4px red") == "none",
          "box_shadow_without_blur");

    // Animations.
    {
        AnimationPaint a;
        a.keyframe(0);
        a.property("opacity", "1");
        a.keyframe(1);
        a.property("opacity", "1");
        check(a.result().has_value(), "an animation of opacity counts (the value, not the picture)");
    }
    {
        AnimationPaint a;
        a.keyframe(0);
        a.property("transform", "none");
        check(!a.result(), "an animation of transform does not");
    }
    {
        AnimationPaint a;
        a.keyframe(0);
        a.property("backdropFilter", "blur(2px)");
        check(a.result() && a.result()->backdrop, "camelCase backdropFilter counts as a backdrop");
    }
    {
        AnimationPaint a;
        a.keyframe(0);
        a.property("boxShadow", "1px 1px 0 red");
        a.keyframe(1);
        a.property("box-shadow", "3px 3px 0 red");
        check(!a.result(), "box-shadow without blur, both ends given");
        AnimationPaint b;
        b.keyframe(1);
        b.property("box-shadow", "3px 3px 0 red");
        check(b.result().has_value(), "box-shadow with an implicit first keyframe (the box's own value) counts");
        AnimationPaint c;
        c.keyframe(0);
        c.property("-webkit-box-shadow", std::nullopt);
        c.keyframe(1);
        c.property("box-shadow", "0 0 0 red");
        check(c.result().has_value(), "a value that is no string counts");
    }

    // The counter.
    {
        PaintCounter<int> c;
        for (int i = 0; i < twidget::kMaxHeavyBoxes; i++) c.set_box(i, BoxPaint{true, false});
        check(!c.check() && c.heavy() == 16, "16 heavy boxes are fine");
        c.set_box(100, BoxPaint{true, false});
        auto v = c.check();
        check(v && v->code == twidget::PaintLimit::Paint && v->count == 17, "the 17th: paint-limit, count 17");
        c.remove_box(100);
        check(c.check() && c.check()->count == 17, "the first verdict sticks");
        check(c.peak_heavy() == 17, "peak");
    }
    {
        PaintCounter<int> c;
        c.set_box(1, BoxPaint{true, true});
        c.set_box(2, BoxPaint{true, true});
        check(!c.check(), "two backdrops are fine");
        c.start_animation(3, "a", BoxPaint{true, true});
        check(!c.check(), "an animation on an element that builds no box counts nothing");
        c.set_box(3, BoxPaint{false, false});
        auto v = c.check();
        check(v && v->code == twidget::PaintLimit::Backdrop && v->count == 3, "a third backdrop (an animation of one): backdrop-limit");
    }
    {
        PaintCounter<int> c;
        c.set_box(1, BoxPaint{false, false});
        c.start_animation(1, "fade", BoxPaint{true, false});
        c.start_animation(1, "fade", BoxPaint{true, false});
        check(c.heavy() == 1, "a box counts once however many animations run on it");
        c.end_animation(1, "fade");
        check(c.heavy() == 1 && c.animating(1, "fade"), "started twice, ended once: still running");
        c.end_animation(1, "fade");
        check(c.heavy() == 0 && !c.animating(1, "fade"), "ended");
        c.end_animation(1, "never");
        check(c.heavy() == 0, "ending one that never started changes nothing");
    }
    {
        PaintCounter<int> c;
        c.set_box(1, BoxPaint{true, false});
        c.set_box(2, BoxPaint{true, false});
        c.start_animation(2, "x", BoxPaint{true, false});
        c.begin_scan();
        c.set_box(1, BoxPaint{true, false});
        bool anims = c.end_scan();
        check(c.heavy() == 1 && anims && !c.has_box(2), "a scan forgets boxes it did not see, with their animations");
    }
    // Reasons and codes (twidget/paint_limits.h).
    check(twidget::paint_limit_reason(twidget::PaintLimit::Paint, 17) == "heavy-elements: 17 (max 16)" &&
              twidget::paint_limit_reason(twidget::PaintLimit::Backdrop, 3) == "backdrop-filter: 3 (max 2)",
          "stop reasons");
    check(twidget::paint_limit_code(twidget::PaintLimit::Paint) == "paint-limit" && twidget::paint_limit_code(twidget::PaintLimit::Backdrop) == "backdrop-limit" &&
              twidget::paint_limit_from_code("paint-limit") == twidget::PaintLimit::Paint && !twidget::paint_limit_from_code("other"),
          "codes");

    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
