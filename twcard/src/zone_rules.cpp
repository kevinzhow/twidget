#include "twcard/zone_rules.h"

#include <chrono>
#include <limits>
#include <stdexcept>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace twcard {
namespace {

constexpr auto kNoTransition = std::numeric_limits<int64_t>::max();

class PlatformZones final : public ZoneRules {
public:
    std::optional<Offset> offset(std::string_view zone, int64_t epochMs) const override {
        if (zone == "UTC") return Offset{0, kNoTransition};
#if defined(__APPLE__)
        auto name = CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(zone.data()),
                                            static_cast<CFIndex>(zone.size()), kCFStringEncodingUTF8, false);
        if (!name) return std::nullopt;
        auto tz = CFTimeZoneCreateWithName(kCFAllocatorDefault, name, false);
        CFRelease(name);
        if (!tz) return std::nullopt;
        const CFAbsoluteTime instant = double(epochMs) / 1000 - kCFAbsoluteTimeIntervalSince1970;
        const auto seconds = CFTimeZoneGetSecondsFromGMT(tz, instant);
        const auto transition = CFTimeZoneGetNextDaylightSavingTimeTransition(tz, instant);
        CFRelease(tz);
        const auto until = transition == 0 ? kNoTransition :
            static_cast<int64_t>((transition + kCFAbsoluteTimeIntervalSince1970) * 1000);
        return Offset{static_cast<int64_t>(seconds * 1000), until};
#elif defined(__linux__) && !defined(__ANDROID__) && defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
        using namespace std::chrono;
        const tzdb* database;
        try {
            database = &get_tzdb();
        } catch (const std::runtime_error&) {
            return Offset{0, kNoTransition};
        }
        try {
            const auto* tz = database->locate_zone(zone);
            const auto info = tz->get_info(sys_time<milliseconds>(milliseconds(epochMs)));
            const auto until = info.end.time_since_epoch() >= duration_cast<seconds>(milliseconds::max())
                ? kNoTransition : duration_cast<milliseconds>(info.end.time_since_epoch()).count();
            return Offset{duration_cast<milliseconds>(info.offset).count(), until};
        } catch (const std::runtime_error&) {
            return std::nullopt;
        }
#else
        // SPEC S8.2: use UTC without access to platform rules.
        return Offset{0, kNoTransition};
#endif
    }

    std::string deviceZone() const override {
#if defined(__APPLE__)
        auto tz = CFTimeZoneCopyDefault();
        if (tz) {
            char name[256];
            const bool found = CFStringGetCString(CFTimeZoneGetName(tz), name, sizeof(name), kCFStringEncodingUTF8);
            CFRelease(tz);
            if (found) return name;
        }
#elif defined(__linux__) && !defined(__ANDROID__) && defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
        try {
            return std::string(std::chrono::current_zone()->name());
        } catch (const std::runtime_error&) {
        }
#endif
        return "UTC";
    }
};

} // namespace

std::shared_ptr<const ZoneRules> platformZoneRules() {
    static const auto rules = std::make_shared<const PlatformZones>();
    return rules;
}

} // namespace twcard
