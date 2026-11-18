#pragma once

#include "twidget/json.h"

namespace twidget {

/// The checked card-facing values and per-scheme defaults (SPEC S7.1).
Json theme_params(const Json& params, bool dark);
Json content_safe_area(const Json& inset);

} // namespace twidget
