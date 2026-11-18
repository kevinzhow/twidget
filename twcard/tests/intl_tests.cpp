#include "twcard/intl.h"

#include <chrono>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>

using twcard::IntlProvider;
using twidget::Json;

namespace {

int failures = 0;

class TestZones final : public twcard::ZoneRules {
public:
    std::optional<Offset> offset(std::string_view zone, int64_t) const override {
        if (zone == "Asia/Kathmandu") return Offset{20700000, std::numeric_limits<int64_t>::max()};
        return std::nullopt;
    }

    std::string deviceZone() const override { return "Asia/Kathmandu"; }
};

void check(const IntlProvider& provider, std::string_view request, std::string_view expected) {
    const auto value = provider.format(Json::parse(request).value());
    if (value == expected) return;
    std::fprintf(stderr, "FAIL: %s, got %s, expected %.*s\n", std::string(request).c_str(),
                 value.value_or("(no formatting)").c_str(), int(expected.size()), expected.data());
    ++failures;
}

void checkOption(const IntlProvider& provider, std::string_view request,
                 std::string_view key, std::string_view expected) {
    const auto reply = provider.format(Json::parse(request).value());
    const auto value = reply ? Json::parse(*reply) : std::nullopt;
    if (value && (*value)[key].asString() == expected) return;
    std::fprintf(stderr, "FAIL: %s %.*s, got %s\n", std::string(request).c_str(), int(key.size()), key.data(),
                 reply.value_or("(no options)").c_str());
    ++failures;
}

void checkPart(const IntlProvider& provider, std::string_view request, std::string_view type, std::string_view expected) {
    const auto reply = provider.format(Json::parse(request).value());
    const auto value = reply ? Json::parse(*reply) : std::nullopt;
    if (value) {
        for (const auto& part : value->arr())
            if (part["type"].asString() == type && part["value"].asString() == expected) return;
    }
    std::fprintf(stderr, "FAIL: %s, got %s, missing %.*s %.*s\n", std::string(request).c_str(),
                 reply.value_or("(no parts)").c_str(), int(type.size()), type.data(), int(expected.size()), expected.data());
    ++failures;
}

} // namespace

int main() {
    const auto& fallback = twcard::fallbackIntlProvider();
    check(fallback, R"({"kind":"number","locale":"de-DE","value":1234.5})", "1,234.5");
    check(fallback, R"({"kind":"number","options":{"style":"percent"},"value":0.125})", "13%");
    check(fallback, R"({"kind":"number","options":{"style":"currency","currency":"USD"},"value":1234.5})", "$1,234.50");
    check(fallback, R"({"kind":"number","options":{"notation":"compact"},"value":1234.5})", "1.2K");
    check(fallback, R"({"kind":"date","options":{"timeZone":"UTC"},"value":1704153600000})", "1/2/2024");
    check(fallback, R"({"kind":"relative","options":{"numeric":"auto"},"value":[-1,"day"]})", "yesterday");
    check(fallback, R"({"kind":"plural","value":1})", "one");
    check(fallback, R"({"kind":"list","value":["red","green","blue"]})", "red, green, and blue");
    checkOption(fallback, R"({"kind":"dateOptions","locale":"de-DE","options":{"timeZone":"GMT"}})", "timeZone", "UTC");
    const auto injected = twcard::makeIntlProvider(std::make_shared<TestZones>());
    checkOption(*injected, R"({"kind":"dateOptions"})", "timeZone", "Asia/Kathmandu");
    check(*injected, R"({"kind":"dateOptions","options":{"timeZone":"No/Such_Zone"}})", R"({"error":"RangeError"})");
    check(*injected, R"({"kind":"date","locale":"en-GB","options":{"hour12":false,"timeStyle":"short"},"value":1704200400000})", "18:45");
    checkPart(*injected, R"({"kind":"dateParts","locale":"en-US","options":{"hour":"numeric","timeZoneName":"shortOffset"},"value":1704200400000})",
              "timeZoneName", "GMT+5:45");
#if defined(__linux__) && !defined(__ANDROID__) && defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    {
        const auto zoneRules = twcard::platformZoneRules();
        constexpr int64_t transition = 1710054000000;  // New York spring transition, 2024-03-10T07:00:00Z.
        const auto before = zoneRules->offset("America/New_York", transition - 1);
        const auto after = zoneRules->offset("America/New_York", transition);
        if (!before || !after || before->offsetMs != -18000000 || before->untilMs != transition ||
            after->offsetMs != -14400000 || after->untilMs <= transition) {
            std::fprintf(stderr, "FAIL: the platform offset changes at the DST transition\n");
            ++failures;
        }
    }
#endif
#ifdef TWCARD_ICU4X
    const auto& icu = twcard::defaultIntlProvider();
    check(icu, R"({"kind":"number","locale":"de-DE","value":1234.5})", "1.234,5");
    check(icu, R"({"kind":"number","locale":"de-DE-u-ca-gregory-nu-latn","value":1234.5})", "1.234,5");
    check(icu, R"({"kind":"number","locale":"de-DE","options":{"style":"percent"},"value":0.125})", "13 %");
    check(icu, R"({"kind":"number","locale":"de-DE","options":{"style":"currency","currency":"EUR"},"value":1234.5})", "1.234,50 €");
    check(icu, R"({"kind":"numberParts","locale":"de-DE","value":1234.5})",
          R"([{"type":"integer","value":"1"},{"type":"group","value":"."},{"type":"integer","value":"234"},{"type":"decimal","value":","},{"type":"fraction","value":"5"}])");
    check(icu, R"({"kind":"plural","locale":"ru","value":3})", "few");
    check(icu, R"({"kind":"upper","locale":"tr","value":"istanbul"})", "İSTANBUL");
    check(icu, R"({"kind":"list","locale":"es","value":["España","Suiza","Italia"]})", "España, Suiza e Italia");
    check(icu, R"({"kind":"relative","locale":"es","options":{"numeric":"auto"},"value":[-1,"day"]})", "ayer");
    check(icu, R"({"kind":"number","locale":"en-US","value":"9007199254740993"})", "9,007,199,254,740,993");
    check(icu, R"({"kind":"number","locale":"ja","options":{"notation":"compact"},"value":12000})", "1.2万");
    check(icu, R"({"kind":"number","locale":"en-US","options":{"minimumIntegerDigits":3},"value":7})", "007");
    check(icu, R"({"kind":"number","locale":"en-US","options":{"numberingSystem":"arab"},"value":12})", "١٢");
    check(icu, R"({"kind":"date","locale":"en-US","options":{"timeZone":"UTC"},"value":1704153600000})", "1/2/2024");
    check(icu, R"({"kind":"date","locale":"en-GB","options":{"timeZone":"UTC","timeStyle":"short"},"value":1704200400000})",
          "13:00");
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    check(icu, R"({"kind":"date","locale":"en-GB","options":{"timeZone":"America/New_York","timeStyle":"short"},"value":1704200400000})",
          "08:00");
    checkPart(icu, R"({"kind":"dateParts","locale":"en-US","options":{"timeZone":"America/New_York","hour":"numeric","timeZoneName":"short"},"value":1704200400000})",
              "timeZoneName", "EST");
    checkPart(icu, R"({"kind":"dateParts","locale":"en-US","options":{"timeZone":"America/New_York","hour":"numeric","timeZoneName":"short"},"value":1720008000000})",
              "timeZoneName", "EDT");
#endif
    checkOption(icu, R"({"kind":"numberOptions","locale":"ar-EG"})", "numberingSystem", "arab");
    checkOption(icu, R"({"kind":"dateOptions","locale":"th-TH","options":{"timeZone":"UTC"}})", "calendar", "buddhist");
    checkOption(icu, R"({"kind":"numberOptions","locale":"zz-ZZ"})", "locale", "en-US");
#endif
    return failures != 0;
}
