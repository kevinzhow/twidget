#include "twcard/validator.h"

#include "css_sanitizer.h"
#include "profile.h"
#include "urls.h"
#include "validation.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace twcard {
namespace {

using twidget::Json;
using validation::Id;
using validation::nodeId;

constexpr Id documentId = 1;

struct TextSize {
    size_t units = 0;
    size_t shaped = 0;
};

// S9.6 counts UTF-16 units, including lone surrogates. Json::parse has already checked the input encoding.
TextSize textSize(std::string_view text) {
    TextSize result;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if ((lead & 0xc0) == 0x80) continue;
        const size_t units = lead >= 0xf0 ? 2 : 1;
        result.units += units;
        if (lead > 0xd6 || (lead == 0xd6 && i + 1 < text.size() &&
                           static_cast<unsigned char>(text[i + 1]) >= 0x90))
            result.shaped += units;
    }
    return result;
}

std::string asciiLower(std::string value) {
    for (auto& c : value)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}

Json operation(std::string_view name, Id id) {
    return Json::object().set("op", name).set("id", id);
}

enum class Kind { Document, Element, Text, Comment };

struct Attribute {
    std::string value;
    size_t textUnits = 0;
};

struct Declaration {
    std::string value;
    bool important = false;
    size_t textUnits = 0;
};

struct Node {
    Kind kind = Kind::Element;
    Id parent = 0;
    std::vector<Id> children;
    std::string tag;
    std::string paintTag;
    std::string adopt;
    bool svg = false;
    bool shown = false;
    std::map<std::string, Attribute, std::less<>> attributes;
    std::map<std::string, Declaration, std::less<>> declarations;
    std::string data;
    TextSize size;
    CssSanitizer::Counts sheet;
    unsigned canvasWidth = 300;   // S6.1, HTML canvas element defaults
    unsigned canvasHeight = 150;

    bool stylesheet() const { return paintTag == "style"; }
    bool canvas() const { return !svg && tag == "canvas"; }
};

bool rootElement(const Node& node) {
    return !node.svg && (node.tag == "html" || node.tag == "head" || node.tag == "body");
}

unsigned canvasDimension(std::string_view value, unsigned fallback) {
    // HTML's rules for parsing non-negative integers and the S6.1 bitmap range.
    const auto start = value.find_first_not_of(" \t\r\n\f");
    if (start == std::string_view::npos) return fallback;
    value.remove_prefix(start);
    if (value.starts_with('+')) value.remove_prefix(1);
    if (value.empty()) return fallback;
    unsigned result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec == std::errc::invalid_argument) return fallback;
    return parsed.ec == std::errc::result_out_of_range ? 16384 : std::min(result, 16384u);
}

std::string capReason(std::string_view cap, uint64_t count, uint64_t max) {
    return std::string(cap) + ": " + std::to_string(count) + " (max " + std::to_string(max) + ')';
}

} // namespace

struct Validator::Impl {
    Impl(const Caps& caps, const CssSanitizer& css) : caps(caps), css(css) {
        auto& document = nodes[documentId];
        document.kind = Kind::Document;
        document.shown = true;
    }

    const Caps& caps;
    const CssSanitizer& css;
    std::unordered_map<Id, Node> nodes;
    std::set<Id> dirtySheets;
    std::map<Id, Id> animations;
    Id html = 0, head = 0, body = 0;
    size_t taskChanges = 0;
    size_t documentCount = 0;
    size_t shapedText = 0;

    void violation(std::string_view detail) const { css.diag("protocol-violation", detail); }

    void setAttribute(Batch& out, Id id, Node& node, std::string name,
                      std::optional<Attribute> value, bool replace = false) {
        auto old = node.attributes.find(name);
        if (!value) {
            if (old == node.attributes.end() && !replace) return;
            if (old != node.attributes.end()) node.attributes.erase(old);
            if (node.shown) out.ops.push(operation("rattr", id).set("name", name));
            return;
        }
        if (node.shown && (replace || old == node.attributes.end() || old->second.value != value->value))
            out.ops.push(operation("attr", id).set("name", name).set("value", value->value));
        node.attributes.insert_or_assign(std::move(name), std::move(*value));
    }

    void attribute(Batch& out, Id id, Node& node, std::string name, const Json* raw) {
        if (!node.svg) name = asciiLower(std::move(name));
        // S0 I3: renderer-owned identities and state markers are private to the renderer.
        if (name == "data-twid" || name.starts_with("data-tw-") || name == "data-tg-link" || name == "data-tg-tag") return;
        if (!(node.svg ? profile::svg_attribute(node.tag, name) : profile::html_attribute(node.tag, name))) {
            css.diag("profile", name);
            return;
        }

        std::optional<Attribute> value;
        if (raw) {
            value = Attribute{raw->asString()};
            if (profile::visible_text_attribute(name)) value->textUnits = textSize(value->value).units;
        }
        bool replace = false;
        if (name == "style") {
            replace = !node.declarations.empty();
            node.declarations.clear();
            if (value) {
                CssSanitizer::Counts counts;
                value->value = css.declarationList(value->value, &counts);
                value->textUnits = counts.stringUnits;
            }
        } else if (!node.svg && node.tag == "a" && name == "href") {
            // S9.3 retains the original URL in the card world only.
            const auto checked = value ? urls::check(value->value) : urls::Verdict{};
            const bool link = value && !value->value.empty() && checked.kind != urls::Kind::Fragment;
            setAttribute(out, id, node, "data-tg-link", link ? std::optional(Attribute{}) : std::nullopt);
            if (value) value->value = checked.kind == urls::Kind::Fragment ? checked.url : "#";
        } else if (value && !node.svg && node.tag == "img" && name == "src") {
            const auto checked = urls::check(value->value);
            const bool image = checked.kind == urls::Kind::Image || checked.kind == urls::Kind::Placeholder;
            if (!image || checked.kind == urls::Kind::Placeholder) css.diag("url-blocked", "img src");
            value->value = image ? checked.url : urls::kPlaceholderImage;
        } else if (profile::url_attribute(name)) {
            const bool acceptsFragment = node.svg && (name == "href" || name == "xlink:href") &&
                (node.tag == "use" || node.tag == "linearGradient" || node.tag == "radialGradient");
            if (!acceptsFragment) return;
            if (value) {
                const auto checked = urls::check(value->value);
                if (checked.kind == urls::Kind::Fragment) value->value = checked.url;
                else {
                    css.diag("url-blocked", name);
                    value.reset();
                }
            }
        } else if (value && node.svg && profile::svg_presentation_attribute(name)) {
            auto checked = css.value(name, value->value);
            if (checked) value->value = std::move(*checked);
            else value.reset();
        } else if (node.canvas()) {
            if (name == "width" || name == "height") {
                const unsigned fallback = name == "width" ? 300 : 150;
                auto& side = name == "width" ? node.canvasWidth : node.canvasHeight;
                side = value ? canvasDimension(value->value, fallback) : fallback;
                if (value) value->value = std::to_string(side);
                replace = value.has_value();
            }
        }
        setAttribute(out, id, node, std::move(name), std::move(value), replace);
    }

    void create(Batch& out, Id id, const Json& input, std::string_view op) {
        if (nodes.contains(id)) return violation("create: duplicate id");
        if (nodes.size() - 1 >= caps.trackedNodes) return violation("create: tracked node capacity");
        if (op != "el") {
            if (!input["data"].isString()) return violation("create: missing data");
            auto& node = nodes[id];
            node.kind = op == "text" ? Kind::Text : Kind::Comment;
            node.data = input["data"].asString();
            node.size = textSize(node.data);
            return;
        }

        if (!validation::boundedString(input["tag"]) || input["tag"].empty() || !input["attrs"].isArray() ||
            (input.find("ns") && input["ns"].asString() != "svg"))
            return violation("el: invalid element");
        const bool svg = input["ns"].asString() == "svg";
        const auto tag = svg ? input["tag"].asString() : asciiLower(input["tag"].asString());
        const auto rule = svg ? profile::svg_element(tag) : profile::html_element(tag);
        auto& node = nodes[id];
        node.tag = tag;
        node.svg = svg;
        if (rule.rule == profile::Element::Allow) node.paintTag = tag;
        if (rule.renameTo) node.paintTag = rule.renameTo;
        if (rule.rule == profile::Element::Unknown)
            node.attributes.emplace("data-tg-tag", Attribute{tag});
        if (rule.rule != profile::Element::Allow && rule.rule != profile::Element::NotRendered)
            css.diag("profile", tag);

        for (const auto& attr : input["attrs"].arr()) {
            if (attr.isArray() && attr.size() == 2 && validation::boundedString(attr[0]) && attr[1].isString())
                attribute(out, id, node, attr[0].asString(), &attr[1]);
            else
                violation("el: invalid attribute");
        }
    }

    bool canShow(const Node& node, const Node& parent) const {
        if (!parent.shown || parent.stylesheet()) return false;
        if (node.kind == Kind::Text) return parent.kind != Kind::Document;
        return node.kind == Kind::Element && !node.paintTag.empty() &&
               (!rootElement(node) || !node.adopt.empty());
    }

    void adoptRoot(Id id, Node& node) {
        if (!rootElement(node) || !node.adopt.empty()) return;
        Id* root = nullptr;
        if (node.tag == "html" && node.parent == documentId) root = &html;
        else if (node.tag == "head" && html && node.parent == html) root = &head;
        else if (node.tag == "body" && html && node.parent == html) root = &body;
        if (root && !*root) {
            *root = id;
            node.adopt = node.tag;
        }
    }

    void markSheet(Id id) {
        if (id != 0 && nodes.at(id).stylesheet()) dirtySheets.insert(id);
    }

    Json styleOp(Id id, std::string_view name, const Declaration& declaration) const {
        return operation("style", id).set("name", name).set("value", declaration.value)
            .set("important", declaration.important);
    }

    void sendInsert(Batch& out, Id id, Id parent, Id before) const {
        out.ops.push(operation("ins", id).set("parent", parent).set("before", before ? Json(before) : Json()));
    }

    Id nextShown(Id id) const {
        const auto& siblings = nodes.at(nodes.at(id).parent).children;
        const auto it = std::find(siblings.begin(), siblings.end(), id);
        for (auto next = it + 1; next != siblings.end(); ++next)
            if (nodes.at(*next).shown) return *next;
        return 0;
    }

    void show(Batch& out, Id id, Id before) {
        std::vector<Id> pending{id};
        while (!pending.empty()) {
            const Id current = pending.back();
            pending.pop_back();
            auto& node = nodes.at(current);
            adoptRoot(current, node);
            if (!canShow(node, nodes.at(node.parent))) continue;
            if (node.kind == Kind::Text) {
                if (shapedText + node.size.shaped > caps.complexText) {
                    css.diag("limit-text", "complex text");
                    continue;
                }
                shapedText += node.size.shaped;
                out.ops.push(operation("text", current).set("data", node.data));
            } else {
                Json attrs = Json::array();
                for (const auto& [name, attr] : node.attributes)
                    attrs.push(Json::array().push(name).push(attr.value));
                auto create = operation("el", current).set("tag", node.paintTag).set("attrs", std::move(attrs));
                if (node.svg) create.set("ns", "svg");
                if (!node.adopt.empty()) create.set("adopt", node.adopt);
                out.ops.push(std::move(create));
                for (const auto& [name, declaration] : node.declarations)
                    out.ops.push(styleOp(current, name, declaration));
            }
            node.shown = true;
            sendInsert(out, current, node.parent, current == id ? before : 0);
            if (node.stylesheet()) dirtySheets.insert(current);
            else pending.insert(pending.end(), node.children.rbegin(), node.children.rend());
        }
    }

    std::vector<Id> subtree(Id id) const {
        std::vector<Id> result{id};
        for (size_t i = 0; i < result.size(); ++i) {
            const auto& children = nodes.at(result[i]).children;
            result.insert(result.end(), children.begin(), children.end());
        }
        return result;
    }

    void hide(Batch& out, Id id) {
        if (!nodes.at(id).shown) return;
        out.ops.push(operation("rm", id));
        for (const Id child : subtree(id)) {
            auto& node = nodes.at(child);
            if (node.shown && node.kind == Kind::Text) shapedText -= node.size.shaped;
            node.shown = false;
        }
    }

    void unlink(Id id, Node& node) {
        if (node.parent == 0) return;
        markSheet(node.parent);
        std::erase(nodes.at(node.parent).children, id);
        node.parent = 0;
    }

    void insert(Batch& out, Id id, Node& node, const Json& input) {
        const Id parentId = nodeId(input["parent"], true);
        const Id before = nodeId(input["before"]);
        const auto parent = nodes.find(parentId);
        if (parent == nodes.end() || parent->second.kind == Kind::Text || parent->second.kind == Kind::Comment ||
            (!input["before"].isNull() && (!before || !nodes.contains(before) || nodes.at(before).parent != parentId)))
            return violation("ins: invalid parent or sibling");

        if (!node.adopt.empty()) return violation("ins: cannot move a document element");
        if (before == id) return;

        for (Id ancestor = parentId; ancestor; ancestor = nodes.at(ancestor).parent)
            if (ancestor == id) return violation("ins: cycle");

        unlink(id, node);
        auto& siblings = parent->second.children;
        const auto position = before ? std::find(siblings.begin(), siblings.end(), before) : siblings.end();
        siblings.insert(position, id);
        node.parent = parentId;
        adoptRoot(id, node);
        markSheet(parentId);

        if (!canShow(node, parent->second)) hide(out, id);
        else if (!node.shown) show(out, id, nextShown(id));
        else sendInsert(out, id, parentId, nextShown(id));
    }

    void data(Batch& out, Id id, Node& node, const Json& value) {
        if ((node.kind != Kind::Text && node.kind != Kind::Comment) || !value.isString())
            return violation("data: invalid target or value");
        const auto size = textSize(value.asString());
        if (node.shown && shapedText - node.size.shaped + size.shaped > caps.complexText) {
            css.diag("limit-text", "complex text");
            return;
        }
        if (node.shown) shapedText = shapedText - node.size.shaped + size.shaped;
        node.data = value.asString();
        node.size = size;
        markSheet(node.parent);
        if (node.shown) out.ops.push(operation("data", id).set("data", node.data));
        else if (node.parent && canShow(node, nodes.at(node.parent))) show(out, id, nextShown(id));
    }

    void style(Batch& out, Id id, Node& node, const Json& input) {
        if (!validation::boundedString(input["name"]) || !input["value"].isString() ||
            (input.find("important") && !input["important"].isBool()))
            return violation("style: invalid declaration");

        const auto& rawName = input["name"].asString();
        const auto name = rawName.starts_with("--") ? rawName : asciiLower(rawName);
        if (!name.starts_with("--") && !profile::property(name)) {
            css.diag("profile", name);
            return;
        }
        if (name.starts_with("--") && (name.size() <= 2 || css_serialize_identifier(name) != name))
            return violation("style: invalid custom property name");

        Declaration declaration;
        if (!input["value"].empty()) {
            CssSanitizer::Counts counts;
            auto checked = css.value(name, input["value"].asString(), &counts);
            if (checked) declaration = {std::move(*checked), input["important"].asBool(), counts.stringUnits};
        }
        // Keep deletion overrides when there is an initial style attribute, for subtree replay.
        if (declaration.value.empty() && !node.attributes.contains("style")) node.declarations.erase(name);
        else node.declarations.insert_or_assign(name, declaration);
        if (node.shown) out.ops.push(styleOp(id, name, declaration));
    }

    size_t largestAttribute(const Json& input, std::string_view op) const {
        if (op == "attr" || op == "style") return textSize(input["value"].asString()).units;
        size_t largest = 0;
        if (op == "el") {
            for (const auto& item : input["attrs"].arr())
                largest = std::max(largest, textSize(item[1].asString()).units);
        }
        return largest;
    }

    std::optional<std::string> apply(Batch& out, const Json& input) {
        const auto& op = input["op"].asString();
        const Id id = nodeId(input["id"]);
        if (!id || op.empty()) {
            violation("op: invalid id or operation");
            return std::nullopt;
        }
        const size_t attributeUnits = largestAttribute(input, op);
        const size_t textUnits = op == "text" || op == "data" || op == "comment"
            ? textSize(input["data"].asString()).units : 0;
        if (attributeUnits > caps.attribute || textUnits > caps.textValue) {
            if (caps.stops) return attributeUnits > caps.attribute
                ? capReason("attribute", attributeUnits, caps.attribute)
                : capReason("text", textUnits, caps.textValue);
            css.diag("limit", attributeUnits > caps.attribute ? "attribute" : "text");
            return std::nullopt;
        }
        if (op == "el" || op == "text" || op == "comment") {
            create(out, id, input, op);
            return std::nullopt;
        }
        auto it = nodes.find(id);
        if (it == nodes.end()) {
            violation("op: unknown id");
            return std::nullopt;
        }
        auto& node = it->second;
        if (op == "ins") insert(out, id, node, input);
        else if (op == "rm") {
            if (!node.adopt.empty()) {
                violation("rm: cannot remove a document element");
                return std::nullopt;
            }
            hide(out, id);
            unlink(id, node);
            for (const Id child : subtree(id)) nodes.erase(child);
        } else if (op == "data") data(out, id, node, input["data"]);
        else if (node.kind != Kind::Element) violation("op: expected an element");
        else if (op == "attr" || op == "rattr") {
            if (!validation::boundedString(input["name"]) || (op == "attr" && !input["value"].isString()))
                violation("attr: invalid name or value");
            else
                attribute(out, id, node, input["name"].asString(), op == "attr" ? input.find("value") : nullptr);
        } else if (op == "style") style(out, id, node, input);
        else if (op == "prop" || op == "call") {
            auto checked = validation::elementOperation(input, id);
            if (!checked) violation("op: invalid element property or method");
            else if (node.shown) out.ops.push(std::move(*checked));
        } else if (op == "canvas" && node.canvas() && input["cmds"].isArray()) {
            const auto& commands = input["cmds"];
            if (commands.size() > caps.canvasCommands) {
                const auto reason = capReason("canvas-commands", commands.size(), caps.canvasCommands);
                if (caps.stops) return reason;
                css.diag("limit", reason);
            } else if (node.shown) {
                const auto imageSource = [this](Id source) {
                    const auto it = nodes.find(source);
                    return it != nodes.end() && it->second.shown && !it->second.svg &&
                        (it->second.tag == "img" || it->second.canvas());
                };
                Json checked = Json::array();
                for (const auto& command : commands.arr()) {
                    auto entry = validation::canvasCommand(command, css, imageSource);
                    if (!entry) {
                        violation("canvas: invalid command");
                        continue;
                    }
                    if ((*entry)[0].asString() == "r") {
                        node.canvasWidth = unsigned((*entry)[1].asInt());
                        node.canvasHeight = unsigned((*entry)[2].asInt());
                    }
                    checked.push(std::move(*entry));
                }
                out.canvasCommands += checked.size();
                if (!checked.empty()) out.ops.push(operation("canvas", id).set("cmds", std::move(checked)));
            }
        } else violation("op: unsupported operation");
        return std::nullopt;
    }

    void flushStyles(Batch& out) {
        for (const Id id : dirtySheets) {
            auto it = nodes.find(id);
            if (it == nodes.end() || !it->second.shown || !it->second.stylesheet()) continue;
            auto& node = it->second;
            std::string text;
            for (const Id child : node.children) {
                const auto& entry = nodes.at(child);
                if (entry.kind == Kind::Text) text += entry.data;
            }
            node.sheet = {};
            auto checked = css.stylesheet(text, node.sheet);
            out.ops.push(operation("css", id).set("text", std::move(checked)));
        }
        dirtySheets.clear();
    }

    std::optional<std::string> documentCaps() {
        const auto connected = subtree(documentId);
        documentCount = connected.size() - 1;
        if (!caps.stops) return std::nullopt;
        if (documentCount > caps.nodes) return capReason("nodes", documentCount, caps.nodes);

        uint64_t ruleCount = 0, keyframeCount = 0, pixelCount = 0, textCount = 0;
        for (const Id id : connected) {
            const auto& node = nodes.at(id);
            if (node.kind == Kind::Text && !nodes.at(node.parent).stylesheet()) textCount += node.size.units;
            if (node.canvas()) pixelCount += uint64_t(node.canvasWidth) * node.canvasHeight;
            for (const auto& [name, attr] : node.attributes) textCount += attr.textUnits;
            for (const auto& [name, declaration] : node.declarations) textCount += declaration.textUnits;
            if (node.shown && node.stylesheet()) {
                ruleCount += node.sheet.rules;
                keyframeCount += node.sheet.keyframes;
                textCount += node.sheet.stringUnits;
            }
        }
        if (textCount > caps.documentText) return capReason("document-text", textCount, caps.documentText);
        if (ruleCount > caps.cssRules) return capReason("css-rules", ruleCount, caps.cssRules);
        if (keyframeCount > caps.keyframes) return capReason("keyframes", keyframeCount, caps.keyframes);
        if (pixelCount > caps.canvasPixels) return capReason("canvas-pixels", pixelCount, caps.canvasPixels);
        return std::nullopt;
    }
};

Validator::Validator(ValidatorOptions options, Diag diag) : diag_(std::move(diag)) {
    if (options.compat) {
        caps_.textValue = 2 * 1024 * 1024;
        caps_.attribute = 4 * 1024 * 1024;
        caps_.domChanges = 200000;
        caps_.canvasCommands = 500000;
        caps_.complexText = std::numeric_limits<size_t>::max();
        caps_.stops = false;
    }
    css_ = std::make_unique<CssSanitizer>(diag_);
    impl_ = std::make_unique<Impl>(caps_, *css_);
}

Validator::~Validator() = default;

void Validator::beginTask() { impl_->taskChanges = 0; }

Validator::Batch Validator::ops(std::string_view batchJson) {
    Batch out;
    if (stopped_) return out;
    const auto batch = Json::parse(batchJson);
    if (!batch || !batch->isArray()) {
        impl_->violation("ops: expected an array");
        return out;
    }

    auto stop = [this](std::string reason) {
        stopped_ = std::move(reason);
        css_->diag("limit", *stopped_);
        Batch result;
        result.stop = stopped_;
        return result;
    };

    if (impl_->taskChanges + batch->size() > caps_.domChanges) {
        auto reason = capReason("dom-changes", impl_->taskChanges + batch->size(), caps_.domChanges);
        if (caps_.stops) return stop(std::move(reason));
        css_->diag("limit", reason);
        return out;
    }
    impl_->taskChanges += batch->size();

    for (const auto& input : batch->arr())
        if (auto reason = impl_->apply(out, input)) return stop(std::move(*reason));
    impl_->flushStyles(out);
    if (auto reason = impl_->documentCaps()) return stop(std::move(*reason));
    return out;
}

bool Validator::renderedElement(int64_t id) const {
    const auto it = impl_->nodes.find(id);
    return it != impl_->nodes.end() && it->second.kind == Kind::Element && it->second.shown;
}

size_t Validator::documentNodes() const { return impl_->documentCount; }
size_t Validator::trackedNodes() const { return impl_->nodes.size() - 1; }

bool Validator::supports(std::string_view property, std::string_view value) const {
    return css_->supportsDeclaration(property, value);
}

bool Validator::supportsCondition(std::string_view condition) const {
    return css_->supportsCondition(condition);
}

std::optional<Json> Validator::listen(std::string_view json) {
    if (stopped_) return std::nullopt;
    return validation::eventList(json, {}, *css_);
}

std::optional<Json> Validator::preventDefault(std::string_view json) {
    if (stopped_) return std::nullopt;
    return validation::eventList(json, [this](Id id) { return renderedElement(id); }, *css_);
}

std::optional<Json> Validator::query(std::string_view json, std::string& error) {
    if (stopped_) {
        error = "dead";
        return std::nullopt;
    }
    const auto target = [this](Id id, std::string_view kind) {
        if (!renderedElement(id)) return false;
        const auto& node = impl_->nodes.at(id);
        if (kind == "canvasCall") return node.canvas();
        return kind != "svg" || node.svg;
    };
    auto checked = validation::layoutQuery(json, *css_, target);
    if (!checked) {
        error = "invalid query";
        impl_->violation("query: invalid name, target or arguments");
    } else {
        error.clear();
    }
    return checked;
}

std::optional<Json> Validator::anim(std::string_view json) {
    if (stopped_) return std::nullopt;
    const auto input = Json::parse(json);
    constexpr size_t maxRecords = 4096;  // S6.2
    if (!input || !input->isArray() || input->size() > maxRecords) {
        impl_->violation("anim: invalid record list");
        return std::nullopt;
    }

    Json result = Json::array();
    for (const auto& record : input->arr()) {
        const Id aid = validation::positiveId(record["aid"]);
        const auto active = impl_->animations.find(aid);
        const Id target = nodeId(record["target"]);
        auto checked = validation::animationRecord(record, *css_);
        if (checked && !(*checked)["remove"].asBool() && !renderedElement(target)) checked.reset();
        if (!checked) impl_->violation("anim: invalid animation record");
        if (!checked || (*checked)["remove"].asBool()) {
            if (active != impl_->animations.end()) {
                result.push(Json::object().set("aid", aid).set("remove", true));
                impl_->animations.erase(active);
            }
            continue;
        }
        const bool scriptAnimation = (*checked)["keyframes"].isArray();
        const bool wasScriptAnimation = active != impl_->animations.end() && active->second != 0;
        if (scriptAnimation && !wasScriptAnimation && caps_.stops) {
            const auto kept = std::count_if(impl_->animations.begin(), impl_->animations.end(),
                                            [](const auto& item) { return item.second != 0; });
            if (size_t(kept) >= caps_.liveAnimations) {
                css_->diag("animation-limit", "script animations");
                continue;
            }
        }
        impl_->animations.insert_or_assign(aid, scriptAnimation ? target : 0);
        result.push(std::move(*checked));
    }
    if (result.empty()) return std::nullopt;
    return result;
}

} // namespace twcard
