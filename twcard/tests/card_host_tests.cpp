#include "twcard/card_host.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>

using twidget::Json;
using namespace twcard;
using namespace std::chrono_literals;

namespace {

int failures = 0;

void check(bool condition, const char* description) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", description);
    ++failures;
}

class RendererFixture final : public Renderer {
public:
    void load() override {
        seen.push_back("load");
        onMessage(R"({"type":"ready"})");
    }

    void send(const std::string& wire) override {
        auto msg = Json::parse(wire).value();
        const auto& kind = msg["type"].asString();
        seen.push_back(kind);
        if (kind == "ops") {
            ops = msg["ops"];
            onMessage(R"({"type":"size","height":140,"innerWidth":400,"innerHeight":140,"dpr":1})");
        } else if (kind == "query") {
            if (!holdQuery || msg["qid"].asNumber() < 0) {
                const auto reply = Json::object().set("type", "result").set("qid", msg["qid"])
                    .set("result", Json::object().set("innerWidth", 400));
                onMessage(reply.dump());
            } else {
                queryHeld = true;
            }
        }
    }

    void post(std::function<void()> work) override {
        {
            std::lock_guard lock(mutex);
            queue.push_back(std::move(work));
        }
        ready.notify_one();
    }

    void destroy() override { destroyed = true; }

    bool until(const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!done()) {
            std::function<void()> work;
            {
                std::unique_lock lock(mutex);
                if (!ready.wait_until(lock, deadline, [this] { return !queue.empty(); })) return done();
                work = std::move(queue.front());
                queue.pop_front();
            }
            work();
        }
        return true;
    }

    std::vector<std::string> seen;
    Json ops;
    bool destroyed = false, holdQuery = false, queryHeld = false;

private:
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::function<void()>> queue;
};

} // namespace

int main() {
    auto resources = std::make_shared<Resources>();
    std::ifstream bundle(std::string(TWCARD_TEST_BUNDLES) + "/test-runtime.js");
    std::ostringstream source;
    source << bundle.rdbuf();
    resources->bundle = source.str();
    // A fixture bundle that takes its script from the host's boot HTML for the S10 tests.
    const std::string configScripts = "scripts = json().config.scripts || [];";
    const auto start = resources->bundle.find(configScripts);
    if (start == std::string::npos) return 1;
    resources->bundle.replace(start, configScripts.size(), "scripts = [{text: json().html}];");

    RendererFixture* renderer = nullptr;
    CardHost host(R"(
        sendOps([{op:'el', id:2, tag:'html', attrs:[]},{op:'ins', id:2,parent:1},
          {op:'el',id:3, tag:'body', attrs:[]},{op:'ins',id:3,parent:2}]);
        document.title = JSON.parse(query({q:'viewport'})).innerWidth + ':';
    )", {}, 400, {}, resources, [&](const Theme&, double) {
        auto fixture = std::make_unique<RendererFixture>();
        renderer = fixture.get();
        return fixture;
    });

    Diagnostics diagnostics;
    std::vector<std::string> lifecycle;
    host.onChange = [&](const Diagnostics& d) {
        if (!diagnostics.loaded && d.loaded) lifecycle.push_back("loaded");
        if (!diagnostics.rendered && d.rendered) lifecycle.push_back("rendered");
        if (!diagnostics.dead && d.dead) lifecycle.push_back("dead");
        diagnostics = d;
    };
    host.start();
    check(renderer->until([&] { return diagnostics.rendered; }), "card loaded and hit the renderer acknowledgement");
    const std::vector<std::string> first{"theme", "safeArea", "policy", "load"};
    check(renderer->seen.size() >= first.size() &&
          std::equal(first.begin(), first.end(), renderer->seen.begin()), "initial renderer messages have the specified order");
    check(lifecycle == std::vector<std::string>{"loaded", "rendered"} && diagnostics.firstContentMs.has_value() &&
          diagnostics.height == 140 && renderer->ops.size() == 4, "validated content, loaded and rendered");
    check(host.probe("document.title") == "400:[loaded]", "query returned the renderer viewport within the JS task");

    renderer->onMessage(R"({"type":"limit","code":"paint-limit","count":17})");
    check(renderer->until([&] { return diagnostics.dead.has_value(); }) &&
          diagnostics.dead == "heavy-elements: 17 (max 16)" && renderer->seen.back() == "frozen",
          "strict paint limit stops and freezes the card");
    host.destroy();
    check(renderer->destroyed && host.probe("1") == "Error: not running", "destroy frees the renderer and ends probes");

    CardHost waiting("query({q:'viewport'});", {}, 400, {}, resources, [&](const Theme&, double) {
        auto fixture = std::make_unique<RendererFixture>();
        fixture->holdQuery = true;
        renderer = fixture.get();
        return fixture;
    });
    waiting.start();
    check(renderer->until([&] { return renderer->queryHeld; }), "JS thread blocked on a renderer query");
    const auto before = std::chrono::steady_clock::now();
    waiting.destroy();
    check(renderer->destroyed && std::chrono::steady_clock::now() - before < 500ms,
          "destroy wakes a blocked query without waiting for its timeout");

    return failures != 0;
}
