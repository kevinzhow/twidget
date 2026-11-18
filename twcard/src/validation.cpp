#include "validation.h"

#include "css_sanitizer.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <set>
#include <string>

namespace twcard::validation {
namespace {

using twidget::Json;

constexpr size_t maxString = 2048;  // S9.7
constexpr size_t maxNumbers = 4096; // S9.7
constexpr auto maxIntId = std::numeric_limits<int32_t>::max();

bool oneOf(std::string_view name, std::initializer_list<std::string_view> values) {
    return std::find(values.begin(), values.end(), name) != values.end();
}

bool finite(const Json& value, double magnitude = std::numeric_limits<double>::max()) {
    return value.isNumber() && std::abs(value.asNumber()) <= magnitude;
}

bool integer(const Json& value, double lo, double hi) {
    if (!value.isNumber()) return false;
    const double n = value.asNumber();
    return n >= lo && n <= hi && std::trunc(n) == n;
}

bool numbers(const Json& list, size_t lo, size_t hi) {
    return list.isArray() && list.size() >= lo && list.size() <= hi &&
        std::all_of(list.arr().begin(), list.arr().end(), [](const Json& n) { return finite(n); });
}

bool numbersFrom(const Json& list, size_t first) {
    return std::all_of(list.arr().begin() + first, list.arr().end(),
                       [](const Json& n) { return finite(n); });
}

bool reference(const Json& value, const char* key) {
    return value.isObject() && value.size() == 1 && positiveId(value[key]);
}

bool fillRule(const Json& value) {
    return oneOf(value.asString(), {"nonzero", "evenodd"});
}

bool forwarded(std::string_view type) {
    // S3.1–S3.2 and the public guide's Input list.
    return oneOf(type, {"click", "dblclick", "contextmenu", "load", "error", "toggle",
                        "pointerdown", "pointerup", "pointermove", "pointercancel", "pointerenter",
                        "pointerleave", "pointerover", "pointerout", "gotpointercapture", "lostpointercapture",
                        "mousedown", "mouseup", "mousemove", "mouseenter", "mouseleave", "mouseover", "mouseout",
                        "animationstart", "animationiteration", "animationend", "animationcancel",
                        "transitionrun", "transitionstart", "transitionend", "transitioncancel"});
}

bool pathMethod(std::string_view method, const Json& args) {
    if (method == "closePath") return args.empty();
    if (oneOf(method, {"moveTo", "lineTo"})) return numbers(args, 2, 2);
    if (oneOf(method, {"quadraticCurveTo", "rect"})) return numbers(args, 4, 4);
    if (method == "bezierCurveTo") return numbers(args, 6, 6);
    if (method == "arcTo") return numbers(args, 5, 5) && args[4].asNumber() >= 0;
    if (method == "arc" || method == "ellipse") {
        const size_t count = method == "arc" ? 5 : 7;
        if (args.size() != count && (args.size() != count + 1 || !args[count].isBool())) return false;
        return std::all_of(args.arr().begin(), args.arr().begin() + count,
                           [](const Json& n) { return finite(n); }) &&
            args[2].asNumber() >= 0 && (method != "ellipse" || args[3].asNumber() >= 0);
    }
    if (method == "roundRect") {
        if (numbers(args, 4, 5)) return true;
        if (args.size() != 5 || !numbers(args[4], 1, 4)) return false;
        return std::all_of(args.arr().begin(), args.arr().begin() + 4,
                           [](const Json& n) { return finite(n); });
    }
    return method == "addPath" && reference(args[0], "p") &&
        (args.size() == 1 || (args.size() == 2 && numbers(args[1], 6, 6)));
}

bool drawMethod(std::string_view method, const Json& args, const KnownNode& imageSource) {
    if (oneOf(method, {"save", "restore", "resetTransform", "beginPath"})) return args.empty();
    if (oneOf(method, {"scale", "translate"})) return numbers(args, 2, 2);
    if (method == "rotate") return numbers(args, 1, 1);
    if (method == "transform") return numbers(args, 6, 6);
    if (method == "setTransform")
        return args.empty() || numbers(args, 6, 6) || (args.size() == 1 && numbers(args[0], 6, 6));
    if (oneOf(method, {"fillRect", "strokeRect", "clearRect"})) return numbers(args, 4, 4);
    if (oneOf(method, {"fillText", "strokeText"}))
        return args.size() >= 3 && args.size() <= 4 && boundedString(args[0]) && numbersFrom(args, 1);
    if (method == "drawImage")
        return (args.size() == 3 || args.size() == 5 || args.size() == 9) &&
            reference(args[0], "n") && imageSource(nodeId(args[0]["n"])) && numbersFrom(args, 1);
    if (method == "setLineDash") return args.size() == 1 && numbers(args[0], 0, maxNumbers);
    if (method == "stroke") return args.empty() || (args.size() == 1 && reference(args[0], "p"));
    if (method == "fill" || method == "clip") {
        if (args.empty()) return true;
        if (args.size() == 1) return fillRule(args[0]) || reference(args[0], "p");
        return args.size() == 2 && reference(args[0], "p") && fillRule(args[1]);
    }
    return pathMethod(method, args);
}

std::optional<Json> cssString(const Json& value, std::string_view property, const CssSanitizer& css) {
    if (!boundedString(value)) return std::nullopt;
    auto checked = css.value(property, value.asString());
    if (!checked) return std::nullopt;
    return Json(std::move(*checked));
}

std::optional<Json> canvasState(std::string_view prop, const Json& value, const CssSanitizer& css) {
    if (prop == "fillStyle" || prop == "strokeStyle")
        return reference(value, "g") ? std::optional(value) : cssString(value, "color", css);
    if (prop == "font") return cssString(value, "font", css);

    if (oneOf(prop, {"globalAlpha", "lineWidth", "miterLimit", "lineDashOffset"}))
        return finite(value) ? std::optional(value) : std::nullopt;
    if (prop == "imageSmoothingEnabled") return value.isBool() ? std::optional(value) : std::nullopt;
    const auto& text = value.asString();
    if ((prop == "lineCap" && oneOf(text, {"butt", "round", "square"})) ||
        (prop == "lineJoin" && oneOf(text, {"round", "bevel", "miter"})) ||
        (prop == "textAlign" && oneOf(text, {"start", "end", "left", "right", "center"})) ||
        (prop == "textBaseline" && oneOf(text, {"top", "hanging", "middle", "alphabetic", "ideographic", "bottom"})))
        return value;
    return std::nullopt;
}

bool canvasQuery(std::string_view method, const Json& args) {
    if (method == "getTransform" || method == "getLineDash") return args.empty();
    if (method != "isPointInPath" && method != "isPointInStroke") return false;
    const size_t first = reference(args[0], "p") ? 1 : 0;
    if (!finite(args[first]) || !finite(args[first + 1])) return false;
    if (args.size() == first + 2) return true;
    return method == "isPointInPath" && args.size() == first + 3 && fillRule(args[first + 2]);
}

bool svgQuery(std::string_view method, const Json& args) {
    if (method == "getPointAtLength") return numbers(args, 1, 1);
    if (method == "getSubStringLength")
        return args.size() == 2 && integer(args[0], 0, maxIntId) && integer(args[1], 0, maxIntId);
    return args.empty() && oneOf(method, {"getBBox", "getCTM", "getScreenCTM", "getComputedTextLength",
                                         "getTotalLength", "getNumberOfChars"});
}

// S9.7 animation bounds, in CSS milliseconds.
constexpr double dayMs = 24 * 60 * 60 * 1000;
constexpr double maxRate = 1000;
constexpr size_t maxKeyframes = 64;
constexpr size_t maxValues = 64;

bool composite(std::string_view mode) { return oneOf(mode, {"replace", "add", "accumulate"}); }

std::optional<Json> animationTiming(const Json& source, const CssSanitizer& css) {
    if (!source.isObject()) return std::nullopt;
    const auto& duration = source["duration"];
    const auto& delay = source["delay"];
    const auto& endDelay = source["endDelay"];
    const auto& iterations = source["iterations"];
    const auto& iterationStart = source["iterationStart"];
    const auto& direction = source["direction"].asString();
    const auto& fill = source["fill"].asString();
    if (!finite(duration, dayMs) || duration.asNumber() < 0 || !finite(delay, dayMs) ||
        !finite(endDelay, dayMs) || !source.find("iterations") ||
        (!iterations.isNull() && (!finite(iterations) || iterations.asNumber() < 0)) ||
        !finite(iterationStart) || iterationStart.asNumber() < 0 ||
        !oneOf(direction, {"normal", "reverse", "alternate", "alternate-reverse"}) ||
        !oneOf(fill, {"none", "forwards", "backwards", "both", "auto"}) ||
        !boundedString(source["easing"]))
        return std::nullopt;

    auto easing = css.easing(source["easing"].asString());
    if (!easing) return std::nullopt;
    return Json::object().set("duration", duration).set("delay", delay).set("endDelay", endDelay)
        .set("iterations", iterations).set("iterationStart", iterationStart).set("direction", direction)
        .set("fill", fill).set("easing", std::move(*easing));
}

std::string lower(std::string name) {
    for (char& c : name)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return name;
}

std::optional<Json> animationFrames(const Json& source, const CssSanitizer& css) {
    if (!source.isArray() || source.size() > maxKeyframes) return std::nullopt;
    Json frames = Json::array();
    double previous = 0;
    for (const auto& frame : source.arr()) {
        const auto& offset = frame["offset"];
        const auto& mode = frame["composite"].asString();
        const auto& values = frame["values"];
        if (!frame.isObject() || !finite(offset, 1) || offset.asNumber() < previous ||
            (!composite(mode) && mode != "auto") || !values.isObject() || values.size() > maxValues ||
            !boundedString(frame["easing"]))
            return std::nullopt;
        auto easing = css.easing(frame["easing"].asString());
        if (!easing) return std::nullopt;
        previous = offset.asNumber();

        Json cleanValues = Json::object();
        for (const auto& [name, value] : values.obj()) {
            if (!boundedString(value) || name.size() > maxString) return std::nullopt;
            auto property = name.starts_with("--") ? name : lower(name);
            if (property.starts_with("animation-") || property.starts_with("transition-") ||
                oneOf(property, {"animation", "transition", "offset", "easing", "composite", "computed-offset"}))
                continue;
            if (css_serialize_identifier(property) != property) continue;
            if (auto checked = css.value(property, value.asString()))
                cleanValues.set(property, std::move(*checked));
        }
        frames.push(Json::object().set("offset", offset).set("easing", std::move(*easing)).set("composite", mode)
                             .set("values", std::move(cleanValues)));
    }
    return frames;
}

} // namespace

Id positiveId(const Json& value) {
    return integer(value, 1, maxIntId) ? value.asInt64() : 0;
}

Id nodeId(const Json& value, bool allowDocument) {
    // S2.4 reserves 1 for the document and bounds other ids below 2^31.
    const Id id = positiveId(value);
    return id == 1 && !allowDocument ? 0 : id;
}

bool boundedString(const Json& value) {
    return value.isString() && value.size() <= maxString;
}

std::optional<Json> elementOperation(const Json& input, Id id) {
    if (input["op"].asString() == "prop") {
        const auto& name = input["name"].asString();
        if (!oneOf(name, {"scrollLeft", "scrollTop"}) || !finite(input["value"])) return std::nullopt;
        return Json::object().set("op", "prop").set("id", id).set("name", name).set("value", input["value"]);
    }

    const auto& method = input["method"].asString();
    Json noArgs = Json::array();
    const Json& args = input.find("args") ? input["args"] : noArgs;
    if (!args.isArray()) return std::nullopt;
    if (oneOf(method, {"observeResize", "unobserveResize"})) {
        if (!args.empty()) return std::nullopt;
    } else if (oneOf(method, {"scrollTo", "scrollBy"})) {
        if (!numbers(args, 2, 2)) return std::nullopt;
    } else if (oneOf(method, {"setPointerCapture", "releasePointerCapture"})) {
        if (args.size() != 1 || !integer(args[0], 0, maxIntId)) return std::nullopt;
    } else {
        return std::nullopt;
    }
    return Json::object().set("op", "call").set("id", id).set("method", method).set("args", args);
}

std::optional<Json> canvasCommand(const Json& command, const CssSanitizer& css, const KnownNode& imageSource) {
    if (!command.isArray() || command.size() < 2 || command.size() > 11) return std::nullopt;
    const auto& kind = command[0].asString();
    if (kind == "s" && command.size() == 3) {
        auto value = canvasState(command[1].asString(), command[2], css);
        if (value) return Json::array().push("s").push(command[1]).push(std::move(*value));
        return std::nullopt;
    }
    if (kind == "r" && command.size() == 3 && integer(command[1], 0, 16384) && integer(command[2], 0, 16384))
        return command;
    if (kind == "g" && positiveId(command[1])) {
        const auto& gradient = command[2].asString();
        if (((gradient == "linear" && command.size() == 7) || (gradient == "radial" && command.size() == 9)) &&
            numbersFrom(command, 3))
            return command;
    }
    if (kind == "gs" && command.size() == 4 && positiveId(command[1]) &&
        finite(command[2], 1) && command[2].asNumber() >= 0) {
        auto color = cssString(command[3], "color", css);
        if (color) return Json::array().push("gs").push(command[1]).push(command[2]).push(std::move(*color));
    }
    if (kind == "pn" && command.size() == 3 && positiveId(command[1]) &&
        (command[2].isNull() || boundedString(command[2])))
        return command;
    if (kind == "pc" && command.size() >= 3 && positiveId(command[1])) {
        const Json args(Json::Array(command.arr().begin() + 3, command.arr().end()));
        if (pathMethod(command[2].asString(), args)) return command;
    }
    if (kind == "c") {
        const Json args(Json::Array(command.arr().begin() + 2, command.arr().end()));
        if (drawMethod(command[1].asString(), args, imageSource)) return command;
    }
    return std::nullopt;
}

std::optional<Json> eventList(std::string_view json, const KnownNode& preventionTarget, const CssSanitizer& css) {
    const auto input = Json::parse(json);
    if (!input || !input->isArray()) {
        css.diag("protocol-violation", preventionTarget ? "pd: expected an array" : "listen: expected an array");
        return std::nullopt;
    }

    Json output = Json::array();
    std::set<std::string_view> seen;
    for (const auto& entry : input->arr()) {
        if (!boundedString(entry)) continue;
        const auto& text = entry.asString();
        if (preventionTarget) {
            const auto colon = text.find(':');
            if (colon == std::string::npos || !forwarded(std::string_view(text).substr(colon + 1))) continue;
            Id id = 0;
            const auto end = text.data() + colon;
            const auto parsed = std::from_chars(text.data(), end, id);
            if (parsed.ec != std::errc{} || parsed.ptr != end || !preventionTarget(id) ||
                std::to_string(id) != std::string_view(text).substr(0, colon))
                continue;
        } else if (!forwarded(text)) {
            continue;
        }
        if (seen.insert(text).second) output.push(entry);
    }
    return output;
}

std::optional<Json> layoutQuery(std::string_view json, const CssSanitizer& css, const QueryTarget& target) {
    const auto parsed = Json::parse(json);
    if (!parsed || !parsed->isObject()) return std::nullopt;
    const auto& input = *parsed;
    const auto& kind = input["q"].asString();
    Json output = Json::object().set("q", kind);
    if (kind == "viewport") return output;
    if (kind == "elementFromPoint") {
        if (!finite(input["x"]) || !finite(input["y"])) return std::nullopt;
        return output.set("x", input["x"]).set("y", input["y"]);
    }
    if (kind == "matchMedia") {
        if (!boundedString(input["query"])) return std::nullopt;
        auto media = css.mediaQueryList(input["query"].asString());
        return output.set("query", media.value_or("not all"));
    }
    if (kind == "measureText") {
        auto font = cssString(input["font"], "font", css);
        if (!font || !boundedString(input["text"])) return std::nullopt;
        return output.set("font", std::move(*font)).set("text", input["text"]);
    }

    const Id id = nodeId(input["id"]);
    if (!id || !target(id, kind)) return std::nullopt;
    output.set("id", id);
    if (oneOf(kind, {"rect", "rects", "box", "validity"})) return output;
    if (kind == "cs") {
        if (const auto* pseudo = input.find("pseudo")) {
            if (!pseudo->isString() || !oneOf(pseudo->asString(), {"", ":before", ":after", "::before", "::after"}))
                return std::nullopt;
            output.set("pseudo", *pseudo);
        }
        if (const auto* prop = input.find("prop")) {
            if (!boundedString(*prop) || css_serialize_identifier(prop->asString()) != prop->asString())
                return std::nullopt;
            output.set("prop", *prop);
        }
        return output;
    }
    if (kind == "anims") {
        if (input.find("subtree") && !input["subtree"].isBool()) return std::nullopt;
        return output.set("subtree", input["subtree"].asBool());
    }
    if (kind == "svg" || kind == "canvasCall") {
        const auto& method = input["method"].asString();
        const auto& args = input["args"];
        if (!args.isArray() || !(kind == "svg" ? svgQuery(method, args) : canvasQuery(method, args)))
            return std::nullopt;
        return output.set("method", method).set("args", args);
    }
    return std::nullopt;
}

std::optional<Json> animationRecord(const Json& record, const CssSanitizer& css) {
    const Id aid = positiveId(record["aid"]);
    if (!aid) return std::nullopt;
    Json output = Json::object().set("aid", aid);
    if (record.find("remove")) {
        if (!record["remove"].asBool()) return std::nullopt;
        return output.set("remove", true);
    }

    const Id target = nodeId(record["target"]);
    const auto& start = record["startTime"];
    const auto& hold = record["holdTime"];
    if (!target || start.isNull() == hold.isNull() || (!start.isNull() && !finite(start, dayMs)) ||
        (!hold.isNull() && !finite(hold, dayMs)) || !finite(record["playbackRate"], maxRate))
        return std::nullopt;
    output.set("target", target).set("startTime", start).set("holdTime", hold)
        .set("playbackRate", record["playbackRate"]);

    if (const auto* source = record.find("css")) {
        const auto& kind = (*source)["kind"].asString();
        const auto& name = (*source)["name"];
        const auto& origin = (*source)["origin"];
        if (!oneOf(kind, {"animation", "transition"}) || !boundedString(name) ||
            !integer(origin, 0, maxIntId) || (record.find("cancelled") && !record["cancelled"].isBool()))
            return std::nullopt;
        return output.set("css", Json::object().set("kind", kind).set("name", name).set("origin", origin))
            .set("cancelled", record["cancelled"].asBool());
    }

    const auto& mode = record["composite"].asString();
    const auto& iteration = record["iterationComposite"].asString();
    if (!composite(mode) || !oneOf(iteration, {"replace", "accumulate"})) return std::nullopt;
    auto frames = animationFrames(record["keyframes"], css);
    auto timing = animationTiming(record["timing"], css);
    if (!frames || !timing) return std::nullopt;
    return output.set("keyframes", std::move(*frames)).set("timing", std::move(*timing))
        .set("composite", mode).set("iterationComposite", iteration);
}

} // namespace twcard::validation
