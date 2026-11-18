// twcard::Throttle (S1.7): the two sliding-window budgets, their run/defer decisions, the debt cap, the ops cost
// model. Times are given explicitly (ms), so these tests need no clock.
#include "twcard/throttle.h"

#include <cmath>
#include <cstdio>
#include <string>

using twcard::Throttle;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) failures++;
}
bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }
} // namespace

int main() {
    Throttle::Limits limits;  // defaults: S1.6 / S10.1
    check(limits.cpuQuotaMs == 300 && limits.windowMs == 1000 && limits.mainThreadQuotaMs == 100, "default limits");
    check(near(Throttle::opsCostMs(1000, 0), 25) && near(Throttle::opsCostMs(0, 1000), 2) && near(Throttle::opsCostMs(40, 500), 2),
          "opsCostMs: 0.025 ms per op, 0.002 ms per canvas command");

    {
        Throttle t(limits);
        check(t.decide(0).run, "a fresh card runs");
        t.chargeJs(0, 299);
        check(t.decide(10).run, "under the quota: run");
        t.chargeJs(10, 2);
        auto d = t.decide(20);
        check(!d.run, "past the quota: defer");
        // The first bucket (0..100 ms) leaves the window at 1000 ms; then 2 ms remain, under the quota.
        check(d.run || near(d.until, 1000), "deferred until the oldest busy tenth leaves the window (got " + std::to_string(d.until) + ")");
        check(t.decide(1000).run, "runs once it left");
    }
    {
        Throttle t(limits);
        t.chargeJs(0, 5000);  // one huge task
        check(near(t.jsUsed(0), 600), "a debt is at most one quota's worth (window holds 2 x quota)");
        auto d = t.decide(1);
        check(!d.run && near(d.until, 1000), "and it still delays by the window");
    }
    {
        Throttle t(limits);
        for (int i = 0; i < 10; i++) t.chargeJs(i * 100 + 5, 40);  // 400 ms spread over the window
        auto d = t.decide(950);
        // Used 400: the bucket at 0..100 leaves at 1000 (-40 → 360), at 1100 (-40 → 320), at 1200 (-40 → 280 < 300).
        check(!d.run && near(d.until, 1200), "waits until enough tenths left (got " + std::to_string(d.until) + ")");
    }
    {
        Throttle t(limits);
        t.chargeMain(0, 101);
        auto d = t.decide(50);
        check(!d.run && near(d.until, 1000), "the main-thread window defers too");
        t.chargeJs(50, 400);
        d = t.decide(60);
        check(!d.run && near(d.until, 1000), "both: the later of the two");
    }
    {
        Throttle t(limits);
        t.chargeJs(0, 400);
        check(near(t.jsUsed(999), 400) && near(t.jsUsed(1000), 0), "charges leave the window after it");
        check(near(t.jsUsed(-5000), 400) || true, "time going backwards does not crash");
    }
    {
        // cpuKillAfterMs: a card deferred without a break longer than one window for that long.
        Throttle::Limits l = limits;
        l.cpuKillAfterMs = 3000;
        Throttle t(l);
        bool killed = false;
        for (double now = 0; now <= 4000 && !killed; now += 50) {
            t.chargeJs(now, 400);
            auto d = t.decide(now);
            if (!d.run) t.noteDeferred(now);
            killed = t.throttledTooLong(now);
        }
        check(killed, "throttled too long kills once cpuKillAfterMs is set");
        Throttle t2(limits);
        bool killed2 = false;
        for (double now = 0; now <= 10000; now += 50) {
            t2.chargeJs(now, 400);
            if (!t2.decide(now).run) t2.noteDeferred(now);
            killed2 = killed2 || t2.throttledTooLong(now);
        }
        check(!killed2, "never with cpuKillAfterMs 0");
        Throttle t3(l);
        t3.noteDeferred(0);
        t3.noteDeferred(1000);
        // a break longer than one window: the streak starts again
        t3.noteDeferred(2100);
        t3.noteDeferred(3000);
        bool before = t3.throttledTooLong(3500);
        t3.noteDeferred(4000);
        t3.noteDeferred(5000);
        check(!before && t3.throttledTooLong(5100), "a break longer than one window restarts the count");
        check(!t3.throttledTooLong(6200), "and a card that is not deferred any more is not throttled");
    }
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
