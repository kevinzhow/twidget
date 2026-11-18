// SPEC S1.7: a card's two sliding-window budgets and the run/defer decision before each task.
//   JS quota            cpuQuotaMs (300) of JS + validator CPU time per window
//   main-thread window  mainThreadQuotaMs (100) of renderer time per window: layout queries' wall time plus the
//                       modelled cost of what the card sent (opsCostMs)
// Window cpuQuotaWindowMs (1000), tracked in ten tenths. Time past a quota is a debt that delays later tasks, at most
// one quota's worth. All times are ms on one monotonic clock given by the caller. Not thread-safe (the JS thread's).
#pragma once

#include <array>
#include <cstdint>

namespace twcard {

class Throttle {
public:
    struct Limits {
        double cpuQuotaMs = 300;
        double mainThreadQuotaMs = 100;
        double windowMs = 1000;
        double cpuKillAfterMs = 0;  // 0: off
    };
    struct Decision {
        bool run = true;
        double until = 0;  // when !run: the earliest time both budgets allow a task again
    };

    /// The renderer-side cost model of a batch (S1.7): 0.025 ms per op, 0.002 ms per canvas command.
    static double opsCostMs(double ops, double commands) { return ops * 0.025 + commands * 0.002; }

    explicit Throttle(Limits limits);

    void chargeJs(double now, double ms);
    void chargeMain(double now, double ms);
    double jsUsed(double now) const { return js_.used(now); }
    double mainUsed(double now) const { return main_.used(now); }

    /// Whether a (non-event) task may run now.
    Decision decide(double now) const;

    /// A task was deferred at `now` (for cpuKillAfterMs).
    void noteDeferred(double now);
    /// cpuKillAfterMs is set and the card has been deferred, with no break longer than one window, for that long.
    bool throttledTooLong(double now) const;

    const Limits& limits() const { return limits_; }

private:
    class Window {
    public:
        Window(double quota, double windowMs) : quota_(quota), bucketMs_(windowMs / kBuckets) {}
        void charge(double now, double ms);
        double used(double now) const;
        /// The earliest time from `now` on at which used() < quota.
        double freeAt(double now) const;

    private:
        static constexpr int kBuckets = 10;  // SPEC S1.7
        int64_t index(double t) const;
        double quota_, bucketMs_;
        std::array<double, kBuckets> cost_{};
        std::array<int64_t, kBuckets> at_{};  // the bucket index each slot holds
    };
    Limits limits_;
    Window js_, main_;
    double streakStart_ = -1, lastDeferred_ = -1;
};

} // namespace twcard
