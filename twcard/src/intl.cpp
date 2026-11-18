#include "twcard/intl.h"

#include <litehtml/utf8_strings.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <ctime>
#include <limits>
#include <string_view>
#include <vector>

namespace twcard {
namespace {

using twidget::Json;

struct Part {
    std::string type;
    std::string text;
};

using Parts = std::vector<Part>;

std::string join(const Parts& parts, bool asParts) {
    if (asParts) {
        auto out = Json::array();
        for (const auto& [type, text] : parts) out.push(Json::object().set("type", type).set("value", text));
        return out.dump();
    }
    std::string out;
    for (const auto& part : parts) out += part.text;
    return out;
}

void add(Parts& parts, std::string_view type, std::string_view text) {
    if (!text.empty()) parts.push_back({std::string(type), std::string(text)});
}

struct NumberOptions {
    int minInteger;
    int minFraction;
    int maxFraction;
    bool grouping;
    bool percent;
    bool currency;
    bool compact;
};

NumberOptions numberOptions(const Json& options) {
    const auto& style = options["style"].asString();
    const bool currency = style == "currency";
    const bool percent = style == "percent";
    const bool compact = options["notation"].asString() == "compact";
    const int defaults = currency ? 2 : 0;
    const int minimum = std::clamp(options["minimumFractionDigits"].asInt(defaults), 0, 100);
    return {std::clamp(options["minimumIntegerDigits"].asInt(1), 1, 21), minimum,
            std::clamp(options["maximumFractionDigits"].asInt(currency ? 2 : percent ? 0 : compact ? 1 : 3),
                       minimum, 100), options["useGrouping"].asBool(true), percent, currency, compact};
}

std::string fixed(double number, int digits) {
    std::array<char, 512> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number, std::chars_format::fixed, digits);
    return result.ec == std::errc{} ? std::string(buffer.data(), result.ptr) : std::string();
}

Parts fallbackNumber(double value, const Json& options) {
    const auto rules = numberOptions(options);
    Parts parts;
    const bool negative = std::signbit(value);
    const bool accounting = rules.currency && negative && options["currencySign"].asString() == "accounting";
    if (accounting) add(parts, "literal", "(");
    else if (negative && options["signDisplay"].asString() != "never") add(parts, "minusSign", "-");
    else if (options["signDisplay"].asString() == "always") add(parts, "plusSign", "+");

    if (rules.currency) {
        const auto& currency = options["currency"].asString();
        const bool code = options["currencyDisplay"].asString() == "code";
        if (!code && currency == "USD") add(parts, "currency", "$");
        else if (!code && currency == "EUR") add(parts, "currency", "€");
        else if (!code && currency == "GBP") add(parts, "currency", "£");
        else {
            add(parts, "currency", currency);
            add(parts, "literal", " ");
        }
    }
    value = std::abs(value);
    if (rules.percent) value *= 100;
    std::string_view suffix;
    if (rules.compact) {
        constexpr std::array shortNames{"K", "M", "B", "T"};
        constexpr std::array longNames{"thousand", "million", "billion", "trillion"};
        for (size_t index = 0; index < shortNames.size() && value >= 1000; ++index) {
            value /= 1000;
            suffix = options["compactDisplay"].asString() == "long" ? longNames[index] : shortNames[index];
        }
    }

    const double factor = std::pow(10.0, rules.maxFraction);
    const double scaled = value * factor;
    if (scaled <= 9007199254740991.0) value = std::round(scaled) / factor;
    const auto decimal = fixed(value, rules.maxFraction);
    const size_t point = decimal.find('.');
    std::string digits = decimal.substr(0, point);
    if (digits.size() < size_t(rules.minInteger)) digits.insert(0, size_t(rules.minInteger) - digits.size(), '0');
    if (rules.grouping && !rules.compact) {
        size_t end = digits.size() % 3;
        if (!end) end = std::min<size_t>(3, digits.size());
        add(parts, "integer", std::string_view(digits).substr(0, end));
        while (end < digits.size()) {
            add(parts, "group", ",");
            add(parts, "integer", std::string_view(digits).substr(end, 3));
            end += 3;
        }
    } else add(parts, "integer", digits);
    if (point != std::string::npos) {
        auto fraction = std::string_view(decimal).substr(point + 1);
        while (fraction.size() > size_t(rules.minFraction) && fraction.back() == '0') fraction.remove_suffix(1);
        if (!fraction.empty()) {
            add(parts, "decimal", ".");
            add(parts, "fraction", fraction);
        }
    }
    if (!suffix.empty() && options["compactDisplay"].asString() == "long") add(parts, "literal", " ");
    add(parts, "compact", suffix);
    if (rules.percent) add(parts, "percentSign", "%");
    if (accounting) add(parts, "literal", ")");
    return parts;
}

std::string timeZone(const Json& options, const ZoneRules& rules) {
    const auto zone = options.find("timeZone") ? options["timeZone"].asString() : rules.deviceZone();
    if (zone == "GMT" || zone == "Etc/UTC" || zone == "Etc/GMT") return "UTC";
    return zone;
}

struct LocalDate {
    std::tm time;
    std::string zone;
    int64_t offsetMs;
};

std::optional<LocalDate> localizedDate(double milliseconds, const Json& options, const ZoneRules& rules) {
    // ECMA-262 TimeClip and the S8.2 IANA time zone.
    if (!std::isfinite(milliseconds) || std::trunc(milliseconds) != milliseconds || std::abs(milliseconds) > 8.64e15)
        return std::nullopt;
    auto zone = timeZone(options, rules);
    const auto offset = rules.offset(zone, static_cast<int64_t>(milliseconds));
    if (!offset) return std::nullopt;
    const auto local = static_cast<int64_t>(std::floor((milliseconds + double(offset->offsetMs)) / 1000));
    if (local < std::numeric_limits<time_t>::min() || local > std::numeric_limits<time_t>::max()) return std::nullopt;
    const time_t seconds = static_cast<time_t>(local);
    std::tm result{};
    if (!gmtime_r(&seconds, &result)) return std::nullopt;
    return LocalDate{result, std::move(zone), offset->offsetMs};
}

std::string gmtOffset(int64_t offsetMs) {
    if (offsetMs == 0) return "GMT";
    const auto minutes = std::abs(offsetMs / 60000);
    auto result = std::string(offsetMs < 0 ? "GMT-" : "GMT+") + std::to_string(minutes / 60);
    if (const auto remainder = minutes % 60)
        result += (remainder < 10 ? ":0" : ":") + std::to_string(remainder);
    return result;
}

Parts fallbackDate(const LocalDate& local, const Json& options) {
    const auto& date = local.time;
    constexpr std::array months{"January", "February", "March", "April", "May", "June",
                                "July", "August", "September", "October", "November", "December"};
    constexpr std::array weekdays{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    const auto& dateStyle = options["dateStyle"].asString();
    const auto& timeStyle = options["timeStyle"].asString();
    const bool dateFields = options.find("year") || options.find("month") || options.find("day") || options.find("weekday");
    const bool timeFields = options.find("hour") || options.find("minute") || options.find("second");
    const bool time = !timeStyle.empty() || timeFields;
    const bool day = !dateStyle.empty() || dateFields || !time;
    const auto digits = [&](int value, std::string_view key) {
        auto text = std::to_string(value);
        if (text.size() < 2 && options[key].asString() == "2-digit") text.insert(text.begin(), '0');
        return text;
    };

    Parts parts;
    if (day) {
        const auto& weekday = options["weekday"].asString();
        if (!weekday.empty() || dateStyle == "full") {
            auto name = std::string_view(weekdays[size_t(date.tm_wday)]);
            if (weekday == "short") name = name.substr(0, 3);
            if (weekday == "narrow") name = name.substr(0, 1);
            add(parts, "weekday", name);
            add(parts, "literal", ", ");
        }
        const auto& month = options["month"].asString();
        const bool named = month == "long" || month == "short" || month == "narrow" ||
            dateStyle == "full" || dateStyle == "long" || dateStyle == "medium";
        bool ownMonth = options.find("month") || !dateFields || !dateStyle.empty();
        bool ownDay = options.find("day") || !dateFields || !dateStyle.empty();
        bool ownYear = options.find("year") || !dateFields || !dateStyle.empty();
        if (ownMonth) {
            std::string text = digits(date.tm_mon + 1, "month");
            if (named) {
                auto name = std::string_view(months[size_t(date.tm_mon)]);
                if (month == "short" || dateStyle == "medium") name = name.substr(0, 3);
                if (month == "narrow") name = name.substr(0, 1);
                text = name;
            }
            add(parts, "month", text);
        }
        if (ownDay) {
            if (ownMonth) add(parts, "literal", named ? " " : "/");
            add(parts, "day", digits(date.tm_mday, "day"));
        }
        if (ownYear) {
            if (ownDay) add(parts, "literal", named ? ", " : "/");
            else if (ownMonth) add(parts, "literal", named ? " " : "/");
            auto year = digits(date.tm_year + 1900, "year");
            if ((dateStyle == "short" || options["year"].asString() == "2-digit") && year.size() > 2)
                year.erase(0, year.size() - 2);
            add(parts, "year", year);
        }
    }
    if (time) {
        if (day) add(parts, "literal", ", ");
        const bool hour12 = options["hour12"].asBool(true);
        const bool hasHour = !timeStyle.empty() || options.find("hour");
        const bool hasMinute = !timeStyle.empty() || options.find("minute");
        const bool hasSecond = (!timeStyle.empty() && timeStyle != "short") || options.find("second");
        auto twoDigits = [](int value) { return value < 10 ? "0" + std::to_string(value) : std::to_string(value); };
        if (hasHour) {
            const int hour = hour12 ? (date.tm_hour + 11) % 12 + 1 : date.tm_hour;
            add(parts, "hour", digits(hour, "hour"));
        }
        if (hasMinute) {
            if (hasHour) add(parts, "literal", ":");
            add(parts, "minute", hasHour ? twoDigits(date.tm_min) : digits(date.tm_min, "minute"));
        }
        if (hasSecond) {
            if (hasMinute || hasHour) add(parts, "literal", ":");
            add(parts, "second", hasMinute ? twoDigits(date.tm_sec) : digits(date.tm_sec, "second"));
        }
        if (hour12 && hasHour) {
            add(parts, "literal", " ");
            add(parts, "dayPeriod", date.tm_hour < 12 ? "AM" : "PM");
        }
    }
    if (options.find("timeZoneName") || timeStyle == "long" || timeStyle == "full") {
        const auto& zoneStyle = options["timeZoneName"].asString();
        const bool longName = zoneStyle == "long" || timeStyle == "full";
        const bool offsetName = zoneStyle == "shortOffset" || zoneStyle == "longOffset";
        add(parts, "literal", " ");
        add(parts, "timeZoneName", local.zone == "UTC" && !offsetName
            ? (longName ? "Coordinated Universal Time" : "UTC") : gmtOffset(local.offsetMs));
    }
    return parts;
}

std::string latinCase(const std::string& text, bool upper) {
    std::string out;
    for (int cursor = 0; size_t(cursor) < text.size();) {
        const int start = cursor;
        const char32_t cp = litehtml::read_utf8_char(text, cursor);
        if (cursor == start) ++cursor;
        char32_t mapped = cp;
        if (upper && ((cp >= 'a' && cp <= 'z') || (cp >= 0xe0 && cp <= 0xfe && cp != 0xf7))) mapped -= 32;
        else if (!upper && ((cp >= 'A' && cp <= 'Z') || (cp >= 0xc0 && cp <= 0xde && cp != 0xd7))) mapped += 32;
        if (mapped == cp) out.append(text, size_t(start), size_t(cursor - start));
        else if (mapped < 128) out.push_back(char(mapped));
        else {
            // ISO-8859-1 changes are two UTF-8 bytes.
            out.push_back(char(0xc0 | (mapped >> 6)));
            out.push_back(char(0x80 | (mapped & 0x3f)));
        }
    }
    return out;
}

bool isParts(std::string_view kind) { return kind == "numberParts" || kind == "dateParts"; }

std::optional<Json> resolved(std::string_view kind, const Json& opts, const ZoneRules& rules) {
    if (kind == "numberOptions") {
        const auto options = numberOptions(opts);
        const auto& s = opts["style"].asString();
        auto out = Json::object().set("locale", "en-US").set("numberingSystem", "latn")
            .set("style", s.empty() ? "decimal" : s)
            .set("minimumIntegerDigits", options.minInteger).set("minimumFractionDigits", options.minFraction)
            .set("maximumFractionDigits", options.maxFraction).set("useGrouping", options.grouping)
            .set("notation", options.compact ? "compact" : "standard")
            .set("signDisplay", opts.find("signDisplay") ? opts["signDisplay"] : Json("auto"));
        if (options.currency) {
            out.set("currency", opts["currency"]).set("currencyDisplay", opts.find("currencyDisplay") ? opts["currencyDisplay"] : Json("symbol"))
                .set("currencySign", opts.find("currencySign") ? opts["currencySign"] : Json("standard"));
        }
        return out;
    }
    if (kind == "dateOptions") {
        const auto zone = timeZone(opts, rules);
        if (zone.empty() || !rules.offset(zone, 0))
            return Json::object().set("error", "RangeError");
        auto out = Json::object().set("locale", "en-US").set("calendar", "gregory").set("numberingSystem", "latn")
            .set("timeZone", zone);
        constexpr std::array fields{"weekday", "era", "year", "month", "day", "hour", "minute", "second",
                                    "timeZoneName", "dateStyle", "timeStyle", "hour12", "hourCycle"};
        for (auto field : fields)
            if (const auto* value = opts.find(field)) out.set(field, *value);
        if (out.size() == 4) out.set("year", "numeric").set("month", "numeric").set("day", "numeric");
        return out;
    }
    return std::nullopt;
}

class Fallback final : public IntlProvider {
public:
    explicit Fallback(std::shared_ptr<const ZoneRules> rules) : rules_(std::move(rules)) {}

    std::optional<std::string> format(const Json& request) const override {
        const auto& kind = request["kind"].asString();
        const auto& options = request["options"];
        const auto& value = request["value"];
        if (auto result = resolved(kind, options, *rules_)) return result->dump();
        if (kind == "number" || kind == "numberParts") {
            if (value.isString()) {
                const auto& text = value.asString();
                if (text == "NaN") return join({{"nan", "NaN"}}, isParts(kind));
                if (text == "Infinity") return join({{"infinity", "∞"}}, isParts(kind));
                if (text == "-Infinity") return join({{"minusSign", "-"}, {"infinity", "∞"}}, isParts(kind));
                if (text == "-0") return join(fallbackNumber(-0.0, options), isParts(kind));
                const std::string_view digits = std::string_view(text).substr(text.starts_with('-') ? 1 : 0);
                if (digits.empty() || digits.find_first_not_of("0123456789") != std::string_view::npos)
                    return std::nullopt;
                auto parts = fallbackNumber(text.starts_with('-') ? -0.0 : 0.0, options);
                auto first = std::find_if(parts.begin(), parts.end(), [](const Part& part) { return part.type == "integer"; });
                if (first != parts.end()) {
                    const auto rules = numberOptions(options);
                    std::string integer(digits);
                    if (rules.percent && integer != "0") integer += "00";
                    if (integer.size() < size_t(rules.minInteger))
                        integer.insert(0, size_t(rules.minInteger) - integer.size(), '0');
                    Parts groups;
                    const std::string_view view(integer);
                    size_t offset = rules.grouping && !rules.compact ? (view.size() - 1) % 3 + 1 : view.size();
                    add(groups, "integer", view.substr(0, offset));
                    for (; offset < view.size(); offset += 3) {
                        add(groups, "group", ",");
                        add(groups, "integer", view.substr(offset, 3));
                    }
                    const auto position = parts.erase(first);
                    parts.insert(position, groups.begin(), groups.end());
                }
                return join(parts, isParts(kind));
            }
            if (!value.isNumber()) return std::nullopt;
            return join(fallbackNumber(value.asNumber(), options), isParts(kind));
        }
        if (kind == "date" || kind == "dateParts") {
            if (!value.isNumber()) return std::nullopt;
            auto date = localizedDate(value.asNumber(), options, *rules_);
            return date ? std::optional(join(fallbackDate(*date, options), isParts(kind))) : std::nullopt;
        }
        if (kind == "plural") {
            if (value.isString() && (value.asString() == "NaN" || value.asString().ends_with("Infinity"))) return "other";
            if (!value.isNumber() && !value.isString()) return std::nullopt;
            const double n = std::abs(value.isNumber() ? value.asNumber() : Json::parseDouble(value.asString().c_str()));
            if (!std::isfinite(n)) return "other";
            if (options["type"].asString() != "ordinal") return n == 1 ? "one" : "other";
            if (std::floor(n) != n) return "other";
            const int last = int(std::fmod(n, 10)), lastTwo = int(std::fmod(n, 100));
            if (lastTwo >= 11 && lastTwo <= 13) return "other";
            return last == 1 ? "one" : last == 2 ? "two" : last == 3 ? "few" : "other";
        }
        if (kind == "compare") {
            if (!value.isArray() || value.size() != 2 || !value[0].isString() || !value[1].isString())
                return std::nullopt;
            const auto a = latinCase(value[0].asString(), false), b = latinCase(value[1].asString(), false);
            if (a != b || options["sensitivity"].asString() == "base") return a < b ? "-1" : a > b ? "1" : "0";
            return value[0].asString() < value[1].asString() ? "1" : value[0].asString() > value[1].asString() ? "-1" : "0";
        }
        if (kind == "upper" || kind == "lower") {
            if (!value.isString()) return std::nullopt;
            return latinCase(value.asString(), kind == "upper");
        }
        if (kind == "list") {
            if (!value.isArray()) return std::nullopt;
            const auto& list = value.arr();
            const auto& type = options["type"].asString();
            const auto& style = options["style"].asString();
            const std::string conjunction = type == "disjunction" ? "or" : style == "short" ? "&" : "and";
            std::string out;
            for (size_t i = 0; i < list.size(); ++i) {
                if (!list[i].isString()) return std::nullopt;
                if (i) {
                    if (type == "unit" || style == "narrow") out += ", ";
                    else if (i + 1 < list.size()) out += ", ";
                    else out += (list.size() > 2 ? ", " : " ") + conjunction + " ";
                }
                out += list[i].asString();
            }
            return out;
        }
        if (kind == "relative") {
            if (!value.isArray() || value.size() != 2 || !value[0].isNumber() || !value[1].isString())
                return std::nullopt;
            const double n = value[0].asNumber();
            const std::string& unit = value[1].asString();
            if (unit == "day" && options["numeric"].asString() == "auto") {
                if (n == -1) return "yesterday";
                if (n == 0) return "today";
                if (n == 1) return "tomorrow";
            }
            std::string amount = join(fallbackNumber(std::abs(n), Json::object()), false) + " " + unit;
            if (std::abs(n) != 1) amount += 's';
            return n < 0 ? amount + " ago" : "in " + amount;
        }
        return std::nullopt;
    }

private:
    std::shared_ptr<const ZoneRules> rules_;
};

#ifdef TWCARD_ICU4X
struct IcuText {
    const char* data;
    size_t size;
    explicit IcuText(std::string_view text) : data(text.data()), size(text.size()) {}
    std::string_view view() const { return size ? std::string_view(data, size) : std::string_view(); }
};

struct IcuOption { IcuText name, value; };

struct IcuRequest {
    IcuText kind, locale;
    const IcuText* args;
    size_t argCount;
    const IcuOption* options;
    size_t optionCount;
    double number;
    int32_t local[7];
    IcuText zone;
    int64_t offsetMs;
};

extern "C" bool twcard_icu4x_format(const IcuRequest*, void (*)(void*, IcuText, IcuText), void*);

class Icu final : public IntlProvider {
public:
    explicit Icu(std::shared_ptr<const ZoneRules> rules) : rules_(std::move(rules)), fallback_(rules_) {}

    std::optional<std::string> format(const Json& request) const override {
        const auto& kind = request["kind"].asString();
        const auto& requested = request["locale"].asString();
        const std::string_view locale = requested.empty() ? "en-US" : requested;
        const auto& value = request["value"];
        const auto& options = request["options"];
        auto resolvedOptions = resolved(kind, options, *rules_);
        if (resolvedOptions && resolvedOptions->find("error")) return resolvedOptions->dump();
        if ((kind == "number" || kind == "numberParts" || kind == "plural") && !value.isNumber() && !value.isString())
            return std::nullopt;
        if (kind == "list" && !value.isArray()) return std::nullopt;
        if (kind == "compare" && (!value.isArray() || value.size() != 2)) return std::nullopt;

        std::vector<IcuText> args;
        if (value.isString()) args.emplace_back(value.asString());
        std::vector<std::pair<std::string_view, std::string>> entries;
        double number = value.asNumber();
        if (kind == "relative") {
            if (!value.isArray() || value.size() != 2 || !value[0].isNumber() || !value[1].isString())
                return std::nullopt;
            number = value[0].asNumber();
            entries.emplace_back("unit", value[1].asString());
        } else for (const auto& entry : value.arr()) {
            if (!entry.isString()) return std::nullopt;
            args.emplace_back(entry.asString());
        }
        for (const auto& [name, setting] : options.obj()) {
            if (setting.isString()) entries.emplace_back(name, setting.asString());
            else if (setting.isNumber() || setting.isBool()) entries.emplace_back(name, setting.dump());
        }
        std::vector<IcuOption> settings;
        for (const auto& [name, setting] : entries) settings.push_back({IcuText(name), IcuText(setting)});
        IcuRequest input{IcuText(kind), IcuText(locale), args.data(), args.size(), settings.data(),
                         settings.size(), number, {}, IcuText(std::string_view{}), 0};
        std::optional<LocalDate> date;
        if (kind == "date" || kind == "dateParts") {
            if (!value.isNumber()) return std::nullopt;
            date = localizedDate(input.number, options, *rules_);
            if (!date) return std::nullopt;
            input.zone = IcuText(date->zone);
            input.offsetMs = date->offsetMs;
            const int milliseconds = int64_t(input.number) % 1000;
            const int nanoseconds = (milliseconds < 0 ? milliseconds + 1000 : milliseconds) * 1000000;
            const auto& time = date->time;
            const int32_t fields[]{time.tm_year + 1900, time.tm_mon + 1, time.tm_mday,
                                   time.tm_hour, time.tm_min, time.tm_sec, nanoseconds};
            std::copy(std::begin(fields), std::end(fields), std::begin(input.local));
        }
        struct Output { Parts parts; bool failed = false; } output;
        const auto append = [](void* opaque, IcuText type, IcuText text) {
            auto& target = *static_cast<Output*>(opaque);
            if (target.failed) return;
            try {
                add(target.parts, type.view(), text.view());
            } catch (...) {
                target.failed = true;
            }
        };
        if (twcard_icu4x_format(&input, append, &output) && !output.failed) {
            if (resolvedOptions) {
                for (const auto& part : output.parts) resolvedOptions->set(part.type, part.text);
                return resolvedOptions->dump();
            }
            return join(output.parts, isParts(kind));
        }
        return resolvedOptions ? std::optional(resolvedOptions->dump()) : fallback_.format(request);
    }

private:
    std::shared_ptr<const ZoneRules> rules_;
    Fallback fallback_;
};
#endif

} // namespace

const IntlProvider& fallbackIntlProvider() {
    static const Fallback provider(platformZoneRules());
    return provider;
}

const IntlProvider& defaultIntlProvider() {
    static const auto provider = makeIntlProvider(platformZoneRules());
    return *provider;
}

std::shared_ptr<const IntlProvider> makeIntlProvider(std::shared_ptr<const ZoneRules> rules) {
#ifdef TWCARD_ICU4X
    return std::make_shared<Icu>(std::move(rules));
#else
    return std::make_shared<Fallback>(std::move(rules));
#endif
}

} // namespace twcard
