#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace twcard {

/// Platform time-zone rules used by the Intl provider (SPEC S8.2).
class ZoneRules {
public:
    struct Offset {
        int64_t offsetMs;
        int64_t untilMs;  // Next transition, exclusive. INT64_MAX if there is none.
    };

    virtual ~ZoneRules() = default;
    virtual std::optional<Offset> offset(std::string_view zone, int64_t epochMs) const = 0;
    virtual std::string deviceZone() const = 0;
};

std::shared_ptr<const ZoneRules> platformZoneRules();

} // namespace twcard
