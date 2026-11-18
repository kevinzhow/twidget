#include "twcard/validator.h"

#include <cstdio>
#include <map>

using twcard::Validator;
using twidget::Json;

namespace {

int failures = 0;

void check(bool condition, const char* description) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", description);
    ++failures;
}

const Json& find(const Json& ops, std::string_view op, int id) {
    static const Json missing;
    for (const auto& item : ops.arr())
        if (item["op"].asString() == op && item["id"].asInt() == id) return item;
    return missing;
}

void document(Validator& validator) {
    const auto batch = validator.ops(R"([
        {"op":"el","id":2,"tag":"html","attrs":[]},
        {"op":"el","id":3,"tag":"head","attrs":[]},
        {"op":"el","id":4,"tag":"body","attrs":[]},
        {"op":"ins","parent":1,"id":2,"before":null},
        {"op":"ins","parent":2,"id":3,"before":null},
        {"op":"ins","parent":2,"id":4,"before":null}
    ])");
    check(!batch.stop && find(batch.ops, "el", 2)["adopt"].asString() == "html" &&
          find(batch.ops, "el", 3)["adopt"].asString() == "head" &&
          find(batch.ops, "el", 4)["adopt"].asString() == "body", "canonical elements adopted");
}

} // namespace

int main() {
    std::map<std::string, int> diagnostics;
    Validator validator({}, [&](std::string_view kind, std::string_view) { ++diagnostics[std::string(kind)]; });
    document(validator);

    auto batch = validator.ops(R"([
        {"op":"el","id":5,"tag":"iframe","attrs":[]},
        {"op":"el","id":6,"tag":"div","attrs":[["style","overflow: auto"],["title","greeting"]]},
        {"op":"text","id":7,"data":"hello"},
        {"op":"ins","parent":4,"id":5},
        {"op":"ins","parent":5,"id":6},
        {"op":"ins","parent":6,"id":7}
    ])");
    check(batch.ops.empty() && validator.documentNodes() == 6 && !validator.renderedElement(6),
          "hidden subtree is tracked without renderer messages");

    batch = validator.ops(R"([{"op":"ins","parent":4,"id":6}])");
    check(validator.renderedElement(6) && find(batch.ops, "text", 7)["data"].asString() == "hello" &&
          find(batch.ops, "ins", 7)["parent"].asInt() == 6 && batch.ops.dump().find("overflow: hidden") != std::string::npos,
          "moving a subtree into view replays its sanitized state");

    batch = validator.ops(R"([
        {"op":"ins","parent":6,"id":4},
        {"op":"ins","parent":7,"id":5},
        {"op":"el","id":6,"tag":"canvas","attrs":[]},
        {"op":"attr","id":77,"name":"class","value":"missing"}
    ])");
    check(batch.ops.empty() && diagnostics["protocol-violation"] == 4, "invalid tree updates are refused");

    batch = validator.ops(R"([
        {"op":"el","id":8,"tag":"style","attrs":[]},
        {"op":"text","id":9,"data":".green { color: green }"},
        {"op":"ins","parent":8,"id":9},
        {"op":"ins","parent":3,"id":8},
        {"op":"el","id":10,"tag":"a","attrs":[["href","https://example.org/"],["ping","https://example.org/"],["onclick","return false"]]},
        {"op":"ins","parent":4,"id":10}
    ])");
    check(!find(batch.ops, "css", 8).isNull() && find(batch.ops, "text", 9).isNull(), "stylesheet text becomes a CSS op");
    const std::string link = find(batch.ops, "el", 10)["attrs"].dump();
    check(link == R"([["data-tg-link",""],["href","#"]])", "external link is marked and its URL stays in the card");

    batch = validator.ops(R"([{"op":"data","id":9,"data":".red { color: red }"},{"op":"rm","id":6}])");
    check(find(batch.ops, "css", 8)["text"].asString().find("red") != std::string::npos &&
          !validator.renderedElement(6) && validator.trackedNodes() == 7, "stylesheet updates and subtree removal");

    std::string error;
    check(validator.query(R"({"q":"rect","id":10,"extra":"discard me"})", error)->dump() == R"({"q":"rect","id":10})" &&
          !validator.query(R"({"q":"rect","id":6})", error), "queries are rebuilt and require a rendered node");
    check(validator.listen(R"(["pointerdown","wheel","click","click"])")->dump() == R"(["pointerdown","click"])" &&
          validator.preventDefault(R"(["10:pointerdown","6:click","10:wheel"])")->dump() == R"(["10:pointerdown"])",
          "interest and prevention use the renderer event set");

    batch = validator.ops(R"([
        {"op":"style","id":10,"name":"color","value":"red"},
        {"op":"rattr","id":10,"name":"style"}
    ])");
    check(find(batch.ops, "rattr", 10)["name"].asString() == "style", "removing inline style clears renderer state");

    batch = validator.ops(R"([
        {"op":"el","id":11,"tag":"canvas","attrs":[]},
        {"op":"ins","parent":4,"id":11},
        {"op":"canvas","id":11,"cmds":[["c","fillRect",0,0,10,10],["c","drawImage",{"n":10},1,1],
                                      ["s","fillStyle","green"],["s","lineWidth","wide"]]}
    ])");
    check(batch.canvasCommands == 2 && find(batch.ops, "canvas", 11)["cmds"].size() == 2,
          "canvas uses checked command types and drawable references");
    check(validator.query(R"({"q":"canvasCall","id":11,"method":"getLineDash","args":[]})", error).has_value() &&
          !validator.query(R"({"q":"canvasCall","id":10,"method":"getLineDash","args":[]})", error),
          "canvas queries require a rendered canvas");
    batch = validator.ops(R"([{"op":"ins","id":11,"parent":5}])");
    check(find(batch.ops, "ins", 11)["parent"].asNumber(-1) == 0 && find(batch.ops, "rm", 11).isNull() &&
          !validator.renderedElement(11), "a canvas moved below a dropped node keeps its detached renderer state");
    batch = validator.ops(R"([{"op":"ins","id":11,"parent":4}])");
    check(find(batch.ops, "ins", 11)["parent"].asInt() == 4 && find(batch.ops, "el", 11).isNull(),
          "reattaching a surviving canvas preserves its renderer identity");
    const size_t known = validator.trackedNodes();
    batch = validator.ops(R"([{"op":"el","id":12,"tag":"span","attrs":[]}])");
    check(batch.ops.empty() && validator.trackedNodes() == known, "a detached creation cannot outlive its batch");

    auto animation = validator.anim(R"([{
        "aid":1,"target":10,"keyframes":[{"offset":0,"easing":"linear","composite":"auto",
          "values":{"color":"blue","animation-name":"another"}}],
        "timing":{"duration":100,"delay":0,"endDelay":0,"iterations":1,"iterationStart":0,
                  "direction":"normal","fill":"none","easing":"linear"},
        "composite":"replace","iterationComposite":"replace","startTime":50,"holdTime":null,"playbackRate":1
    }])");
    check(animation && (*animation)[0]["keyframes"][0]["values"].dump() == R"({"color":"blue"})",
          "animation frames keep only sanitized declarations");
    animation = validator.anim(R"([{"aid":1,"target":10,"keyframes":[]}])");
    check(animation && animation->dump() == R"([{"aid":1,"remove":true}])", "invalid updates remove live animations");

    Validator assembled({}, {});
    batch = assembled.ops(R"([
        {"op":"el","id":2,"tag":"html","attrs":[]},
        {"op":"el","id":4,"tag":"body","attrs":[]},
        {"op":"ins","id":4,"parent":2},
        {"op":"ins","id":2,"parent":1}
    ])");
    check(find(batch.ops, "el", 4)["adopt"].asString() == "body", "canonical nodes may be built while detached");

    Validator capped({}, {});
    document(capped);
    Json operations = Json::array();
    operations.push(Json::object().set("op", "text").set("id", 5).set("data", std::string(32769, 'x')));
    operations.push(Json::object().set("op", "ins").set("parent", 4).set("id", 5));
    batch = capped.ops(operations.dump());
    check(batch.ops.empty() && batch.stop == "text: 32769 (max 32768)" &&
          capped.ops(R"([{"op":"attr","id":4,"name":"class","value":"later"}])").ops.empty(),
          "counted caps withhold the whole batch and all later batches");

    Validator pixels({}, {});
    document(pixels);
    batch = pixels.ops(R"([
        {"op":"el","id":5,"tag":"canvas","attrs":[["width","3000"],["height","3000"]]},
        {"op":"ins","parent":4,"id":5}
    ])");
    check(batch.ops.empty() && batch.stop == "canvas-pixels: 9000000 (max 4194304)", "canvas size uses the document cap");

    Validator sheets({}, {});
    document(sheets);
    std::string css;
    for (int i = 0; i < 401; ++i) css += ".item { color: green } ";
    operations = *Json::parse(R"([
        {"op":"el","id":5,"tag":"style","attrs":[]},
        {"op":"text","id":6},
        {"op":"ins","parent":5,"id":6},
        {"op":"ins","parent":3,"id":5}
    ])");
    operations.arr()[1].set("data", css);
    batch = sheets.ops(operations.dump());
    check(batch.ops.empty() && batch.stop == "css-rules: 401 (max 400)", "stylesheet rule counts are enforced before delivery");

    return failures != 0;
}
