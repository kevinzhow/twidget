#pragma once

#include "twidget/json.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace twcard {

class CssSanitizer;

namespace validation {

using Id = int64_t;
using KnownNode = std::function<bool(Id)>;
using QueryTarget = std::function<bool(Id, std::string_view)>;

Id positiveId(const twidget::Json& value);
Id nodeId(const twidget::Json& value, bool allowDocument = false);
bool boundedString(const twidget::Json& value);

std::optional<twidget::Json> elementOperation(const twidget::Json& input, Id id);
std::optional<twidget::Json> canvasCommand(const twidget::Json& command, const CssSanitizer& css,
                                          const KnownNode& imageSource);
std::optional<twidget::Json> eventList(std::string_view json, const KnownNode& preventionTarget,
                                      const CssSanitizer& css);
std::optional<twidget::Json> layoutQuery(std::string_view json, const CssSanitizer& css,
                                        const QueryTarget& target);
std::optional<twidget::Json> animationRecord(const twidget::Json& record, const CssSanitizer& css);

} // namespace validation
} // namespace twcard
