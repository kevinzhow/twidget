// The accessibility summary (Renderer::accessibility): roles, labels, states and tap points on small documents
// built from ops, as the corpus cards write them (role="button" spans, aria-label, aria-pressed, aria-hidden).
// Text is measured by a stub backend at 8 px per byte.
#include "renderer.h"

#include <cstdio>
#include <string>
#include <vector>

using twidget::Json;
using namespace twlite;
using Node = Renderer::AccessNode;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) failures++;
}

struct StubBackend : Backend {
    int next = 0;
    int create_font(const std::string&, float size, int, bool, const std::string&, FontMetrics& out) override {
        out.ascent = size * 0.8f;
        out.descent = size * 0.2f;
        out.height = size * 1.2f;
        out.x_height = size * 0.5f;
        out.zero_width = 8;
        return ++next;
    }
    void delete_font(int) override {}
    float text_width(int, const std::string& text) override { return 8.0f * float(text.size()); }
    bool load_image(int, const std::string&, float& w, float& h) override {
        w = h = 20;
        return true;
    }
    void canvas_run(int, int, int, const std::vector<CanvasOp>&) override {}
    bool canvas_read(int, int, int, int, int, std::vector<uint8_t>&) override { return false; }
};

Json parse(const std::string& s) {
    auto j = Json::parse(s);
    if (!j) {
        std::printf("bad json: %s\n", s.c_str());
        std::exit(2);
    }
    return *j;
}

/// Ops for a document: html(2) head(3) body(4), a style sheet, then `body` (ops JSON, comma-led or empty).
std::string page(const std::string& css, const std::string& body) {
    return R"({"type":"ops","ops":[
 {"op":"el","id":2,"tag":"html","adopt":"html","attrs":[]},
 {"op":"el","id":3,"tag":"head","adopt":"head","attrs":[]},
 {"op":"el","id":4,"tag":"body","adopt":"body","attrs":[["style","margin:0;font-size:10px;line-height:20px"]]},
 {"op":"el","id":5,"tag":"style","attrs":[]},
 {"op":"css","id":5,"text":)" + Json::quote(css) + R"(},
 {"op":"ins","parent":3,"id":5,"before":null})" + body + "]}";
}

/// An element `id` <tag attrs> under `parent`, with `text` (id + 1000) when not empty.
std::string el(int id, const std::string& tag, const std::string& attrs, int parent, const std::string& text = "") {
    std::string s = R"(,{"op":"el","id":)" + std::to_string(id) + R"(,"tag":")" + tag + R"(","attrs":[)" + attrs + R"(]},{"op":"ins","parent":)" +
                    std::to_string(parent) + R"(,"id":)" + std::to_string(id) + R"(,"before":null})";
    if (!text.empty())
        s += R"(,{"op":"text","id":)" + std::to_string(id + 1000) + R"(,"data":)" + Json::quote(text) + R"(},{"op":"ins","parent":)" + std::to_string(id) +
             R"(,"id":)" + std::to_string(id + 1000) + R"(,"before":null})";
    return s;
}

const Node* find(const std::vector<Node>& nodes, const std::string& label) {
    for (const Node& n : nodes)
        if (n.label == label) return &n;
    return nullptr;
}

std::string dump(const std::vector<Node>& nodes) {
    std::string s;
    for (const Node& n : nodes) s += std::string(Renderer::role_name(n.role)) + ":" + n.label + (n.value.empty() ? "" : "=" + n.value) + " ";
    return s;
}

bool inside(const Node& n) {
    return n.tap_x >= n.box.x && n.tap_x <= n.box.x + n.box.w && n.tap_y >= n.box.y && n.tap_y <= n.box.y + n.box.h;
}

/// The click events a press and release at the node's tap point send, as "click:id ".
std::string tap(Renderer& r, const Node& n) {
    r.touch(0, n.tap_x, n.tap_y, 0, true, true, 0);
    std::string s;
    for (const Json& m : r.touch(2, n.tap_x, n.tap_y, 0, true, true, 0))
        if (m["type"].asString() == "event" && m["ev"]["type"].asString() == "click") s += "click:" + std::to_string(int(m["ev"]["targetId"].asNumber())) + " ";
    return s;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    StubBackend backend;

    // ── a contact card: text blocks, a role="button" span, a cursor: pointer span, chips in a flex row ──
    {
        Renderer r(300, 1, false, backend);
        r.receive(parse(page(".btn{display:inline-block;padding:4px 10px;cursor:pointer} .tags{display:flex} .tag{margin-right:4px}",
                             el(10, "div", R"(["class","card"])", 4) + el(11, "div", R"(["class","name"])", 10, "Ada Lovelace") +
                                 el(12, "div", "", 10, "Writes the   first\nprogram.") + el(13, "div", R"(["class","tags"])", 10) +
                                 el(14, "span", R"(["class","tag"])", 13, "Motion") + el(15, "span", R"(["class","tag"])", 13, "Paris") +
                                 el(16, "div", "", 10) + el(17, "span", R"(["class","btn"],["role","button"])", 16, "Profile") +
                                 el(18, "span", R"(["class","btn"],["id","follow"])", 16, "Follow"))));
        r.receive(parse(R"({"type":"listen","types":["click"]})"));
        r.paint();
        auto nodes = r.accessibility();
        check(dump(nodes) == "text:Ada Lovelace text:Writes the first program. text:Motion text:Paris button:Profile button:Follow ",
              "card: one node per block, white space collapsed, flex items apart, both buttons (got " + dump(nodes) + ")");
        const Node* follow = find(nodes, "Follow");
        const Node* profile = find(nodes, "Profile");
        check(follow && follow->id == 18 && profile && profile->id == 17, "buttons carry their element ids");
        check(follow && inside(*follow) && !(follow->flags & Node::Unreachable), "the tap point is inside the button");
        check(follow && tap(r, *follow) == "click:18 ", "a press at the tap point clicks the button (the pointer path)");
        const Node* name = find(nodes, "Ada Lovelace");
        check(name && name->box.w == 8 * 12 && name->box.h > 0, "a text node's box is its text's (got w " + std::to_string(name ? name->box.w : -1) + ")");

        std::string json = r.accessibility_json();
        uint64_t v = r.accessibility_version();
        check(json.find(R"({"r":"button","id":18,"b":[)") != std::string::npos && json.find(R"("l":"Follow")") != std::string::npos,
              "the JSON names the role, id, box and label");
        // A change that reads the same: the JSON stays, the version does not move.
        r.receive(parse(R"({"type":"ops","ops":[{"op":"style","id":11,"name":"color","value":"red"}]})"));
        r.paint();
        check(r.accessibility_version() == v, "a colour change leaves the summary's version");
        // The script renames the button: a new version.
        r.receive(parse(R"({"type":"ops","ops":[{"op":"data","id":1018,"data":"Following"}]})"));
        r.paint();
        check(r.accessibility_version() == v + 1 && r.accessibility_json().find(R"("l":"Following")") != std::string::npos,
              "new text: a new version with the new label");
    }

    // ── what is not read; labels from aria-label, alt, title; images ──
    {
        Renderer r(300, 1, false, backend);
        r.receive(parse(page(".gone{display:none} .ghost{visibility:hidden} .faded{opacity:0} .b{cursor:pointer;width:40px;height:20px}",
                             el(10, "div", R"(["class","gone"])", 4, "display none") + el(11, "div", R"(["class","ghost"])", 4, "hidden") +
                                 el(12, "div", R"(["class","faded"])", 4, "faded") + el(13, "div", R"(["aria-hidden","true"])", 4, "aria hidden") +
                                 el(14, "button", R"(["aria-label","Play"])", 4, "▶") + el(15, "img", R"(["alt","A bike"],["src","data:image/png;base64,AA=="])", 4) +
                                 el(16, "img", R"(["alt",""],["src","data:image/png;base64,AA=="])", 4) +
                                 el(17, "div", R"(["role","img"],["aria-label","A model"],["style","width:50px;height:50px"])", 4) +
                                 el(18, "div", R"(["class","b"],["title","Close"])", 4) + el(19, "canvas", R"(["width","20"],["height","20"])", 4) +
                                 el(20, "button", "", 4) + el(21, "img", R"(["alt","Next"],["src","data:image/png;base64,AA=="])", 20) +
                                 el(22, "div", R"(["class","b"])", 4))));
        r.paint();
        auto nodes = r.accessibility();
        check(dump(nodes) == "button:Play image:A bike image:A model button:Close button:Next ",
              "hidden content is left out; aria-label, alt, title and a child image's alt name nodes; a nameless pointer area is no button (got " + dump(nodes) + ")");
    }

    // ── headings, links inside a paragraph, states, values, live regions ──
    {
        Renderer r(300, 1, false, backend);
        r.receive(parse(page("",
                             el(10, "h2", "", 4, "Report") + el(11, "div", R"(["role","heading"],["aria-level","4"])", 4, "Details") +
                                 el(12, "p", "", 4, "Read ") + el(13, "a", R"(["href","#more"])", 12, "more") +
                                 el(14, "span", R"(["role","button"],["aria-pressed","true"])", 4, "Shuffle") +
                                 el(15, "span", R"(["role","tab"],["aria-selected","true"])", 4, "Day") +
                                 el(16, "button", R"(["disabled",""])", 4, "Send") +
                                 el(17, "div", R"(["role","progressbar"],["aria-valuenow","40"],["aria-label","Upload"],["style","width:100px;height:4px"])", 4) +
                                 el(18, "progress", R"(["value","0.25"],["aria-label","Steps"])", 4) +
                                 el(19, "details", R"(["open",""])", 4) + el(20, "summary", "", 19, "Ingredients") +
                                 el(21, "div", R"(["aria-live","polite"])", 4) + el(22, "span", "", 21, "3 votes") +
                                 el(23, "span", R"(["role","switch"],["aria-checked","false"])", 4, "Lamp"))));
        r.paint();
        auto nodes = r.accessibility();
        check(dump(nodes) == "heading:Report heading:Details text:Read link:more button:Shuffle tab:Day button:Send progress:Upload=40% progress:Steps=25% button:Ingredients text:3 votes switch:Lamp ",
              "roles in document order (got " + dump(nodes) + ")");
        auto get = [&](const char* l) {
            const Node* n = find(nodes, l);
            return n ? *n : Node{};
        };
        check(get("Report").level == 2 && get("Details").level == 4, "heading levels from the tag and aria-level");
        check((get("Shuffle").flags & (Node::Checkable | Node::Checked)) == (Node::Checkable | Node::Checked), "aria-pressed=true: a pressed toggle");
        check((get("Lamp").flags & (Node::Checkable | Node::Checked)) == Node::Checkable, "aria-checked=false: checkable, not checked");
        check(get("Day").flags & Node::Selected, "aria-selected");
        check(get("Send").flags & Node::Disabled, "a disabled button");
        check((get("Ingredients").flags & (Node::Expandable | Node::Expanded)) == (Node::Expandable | Node::Expanded), "an open <details>' summary is expanded");
        check((get("3 votes").flags & Node::Live) && !(get("Read").flags & Node::Live), "text inside aria-live is live, other text not");

        uint64_t v = r.accessibility_version();
        r.receive(parse(R"({"type":"ops","ops":[{"op":"attr","id":14,"name":"aria-pressed","value":"false"}]})"));
        r.paint();
        auto after = r.accessibility();
        const Node* shuffle = find(after, "Shuffle");
        check(r.accessibility_version() == v + 1 && shuffle && (shuffle->flags & (Node::Checkable | Node::Checked)) == Node::Checkable,
              "setAttribute aria-pressed false: a new version, the toggle released");
    }

    // ── tap points: a button half covered, and one covered whole ──
    {
        Renderer r(300, 1, false, backend);
        r.receive(parse(page(".b{position:absolute;left:0;width:100px;height:40px} .cover{position:absolute;background:red}",
                             el(10, "div", R"(["class","b"],["role","button"],["style","top:0"])", 4, "Half") +
                                 el(11, "div", R"(["class","cover"],["style","left:30px;top:10px;width:40px;height:20px"])", 4) +
                                 el(12, "div", R"(["class","b"],["role","button"],["style","top:50px"])", 4, "Whole") +
                                 el(13, "div", R"(["class","cover"],["style","left:0;top:50px;width:100px;height:40px"])", 4))));
        r.receive(parse(R"({"type":"listen","types":["click"]})"));
        r.paint();
        auto nodes = r.accessibility();
        const Node* half = find(nodes, "Half");
        const Node* whole = find(nodes, "Whole");
        check(half && !(half->flags & Node::Unreachable) && inside(*half) && !(half->tap_x == 50 && half->tap_y == 20),
              "a button covered at its centre is tapped where it shows");
        check(half && tap(r, *half) == "click:10 ", "and that tap clicks it");
        check(whole && (whole->flags & Node::Unreachable), "a button covered whole is marked unreachable");
    }

    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
