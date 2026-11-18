#pragma once

#include "twidget/json.h"
#include "twcard/zone_rules.h"

#include <optional>
#include <string>

namespace twcard {

/// The host's formatting seam (SPEC S8.2). Providers are shared across cards.
class IntlProvider {
public:
    virtual ~IntlProvider() = default;
    virtual std::optional<std::string> format(const twidget::Json& request) const = 0;
};

const IntlProvider& fallbackIntlProvider();
const IntlProvider& defaultIntlProvider();
std::shared_ptr<const IntlProvider> makeIntlProvider(std::shared_ptr<const ZoneRules> rules);

} // namespace twcard
