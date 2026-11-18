// SPEC S1.7; see throttle.h.
#include "twcard/throttle.h"

#include <algorithm>
#include <cmath>

namespace twcard {

int64_t Throttle::Window::index(double t) const { return int64_t(std::floor(t / bucketMs_)); }

double Throttle::Window::used(double now) const {
    const int64_t current = index(now);
    double sum = 0;
    for (int i = 0; i < kBuckets; i++)
        if (cost_[i] > 0 && at_[i] > current - kBuckets && at_[i] <= current) sum += cost_[i];
    return sum;
}

void Throttle::Window::charge(double now, double ms) {
    if (!(ms > 0)) return;
    // The debt is capped at one quota's worth: the window never holds more than twice the quota.
    ms = std::min(ms, std::max(0.0, 2 * quota_ - used(now)));
    if (ms <= 0) return;
    const int64_t current = index(now);
    const int slot = int(((current % kBuckets) + kBuckets) % kBuckets);
    if (at_[slot] != current) {
        at_[slot] = current;
        cost_[slot] = 0;
    }
    cost_[slot] += ms;
}

double Throttle::Window::freeAt(double now) const {
    double sum = used(now);
    if (sum < quota_) return now;
    const int64_t current = index(now);
    // Live buckets oldest first; bucket k leaves the window at (k + kBuckets) * bucketMs.
    for (int64_t k = current - kBuckets + 1; k <= current; k++) {
        const int slot = int(((k % kBuckets) + kBuckets) % kBuckets);
        if (at_[slot] != k || cost_[slot] <= 0) continue;
        sum -= cost_[slot];
        if (sum < quota_) return double(k + kBuckets) * bucketMs_;
    }
    return double(current + 1) * bucketMs_ + bucketMs_ * kBuckets;  // unreachable unless the quota is 0
}

Throttle::Throttle(Limits limits)
    : limits_(limits), js_(limits.cpuQuotaMs, limits.windowMs), main_(limits.mainThreadQuotaMs, limits.windowMs) {}

void Throttle::chargeJs(double now, double ms) { js_.charge(now, ms); }
void Throttle::chargeMain(double now, double ms) { main_.charge(now, ms); }

Throttle::Decision Throttle::decide(double now) const {
    if (js_.used(now) < limits_.cpuQuotaMs && main_.used(now) < limits_.mainThreadQuotaMs) return {};
    return {false, std::max(js_.freeAt(now), main_.freeAt(now))};
}

void Throttle::noteDeferred(double now) {
    if (lastDeferred_ < 0 || now - lastDeferred_ > limits_.windowMs) streakStart_ = now;
    lastDeferred_ = now;
}

bool Throttle::throttledTooLong(double now) const {
    if (limits_.cpuKillAfterMs <= 0 || lastDeferred_ < 0) return false;
    if (now - lastDeferred_ > limits_.windowMs) return false;
    return now - streakStart_ >= limits_.cpuKillAfterMs;
}

} // namespace twcard
