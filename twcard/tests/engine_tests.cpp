// twcard::Engine against SPEC S1 (E1-E8 at the engine level, with our test bundle tests/bundles/test-runtime.js),
// plus timers, frames, visibility, probe and the bytecode/version checks. The card world here is the test bundle; the
// real runtime's conformance cards run through the C ABI (twidget-flutter).
#include "twcard/engine.h"
#include "twidget/json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace twcard;
using twidget::Json;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) failures++;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool wait_for(const std::function<bool()>& pred, double timeoutMs = 5000) {
    auto t0 = std::chrono::steady_clock::now();
    while (!pred()) {
        if (ms_since(t0) > timeoutMs) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

struct Delegate : EngineDelegate {
    std::mutex m;
    std::vector<std::pair<std::string, std::string>> diags;
    std::vector<std::string> errors;
    std::vector<std::string> deaths;
    std::atomic<int> frames{0}, queries{0};
    std::atomic<bool> isLoaded{false};
    double queryDelayMs = 0;
    std::atomic<bool> aborted{false};

    HostValue hostCall(std::string_view kind, std::string_view arg, double& mainMs) override {
        if (kind == "diag") {
            std::string a(arg);
            size_t c = a.find(':');
            std::string k = a.substr(0, c), d = c == std::string::npos ? "" : a.substr(c + 1);
            std::lock_guard l(m);
            diags.emplace_back(k, d);
            return {};
        }
        if (kind == "query") {
            queries++;
            if (queryDelayMs > 0) {
                auto t0 = std::chrono::steady_clock::now();
                while (ms_since(t0) < queryDelayMs && !aborted) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            mainMs = 0.1;
            return HostValue::string("{\"ok\":true}");
        }
        return {};
    }
    void requestFrame() override { frames++; }
    void loaded() override { isLoaded = true; }
    void dead(const std::string& reason) override {
        std::lock_guard l(m);
        deaths.push_back(reason);
    }
    void diagnostic(std::string_view kind, std::string_view detail) override {
        std::lock_guard l(m);
        diags.emplace_back(std::string(kind), std::string(detail));
    }
    void uncaughtError(const std::string& text) override {
        std::lock_guard l(m);
        errors.push_back(text);
    }
    void abortBlocking() override { aborted = true; }

    int diagCount(const std::string& kind) {
        std::lock_guard l(m);
        int n = 0;
        for (auto& d : diags) n += d.first == kind;
        return n;
    }
    std::string death() {
        std::lock_guard l(m);
        return deaths.empty() ? "" : deaths[0];
    }
    size_t deathCount() {
        std::lock_guard l(m);
        return deaths.size();
    }
    std::string errorsJoined() {
        std::lock_guard l(m);
        std::string s;
        for (auto& e : errors) s += e + "|";
        return s;
    }
};

std::shared_ptr<Resources> g_bundle;

std::string boot_arg(const std::vector<std::pair<std::string, bool>>& scripts) {
    Json list = Json::array();
    for (auto& [text, module] : scripts) list.push(Json::object().set("text", text).set("module", module));
    return Json::object().set("html", "").set("config", Json::object().set("scripts", std::move(list))).dump();
}

/// A started engine over the test bundle with `scripts` as the card's inline scripts.
struct Card {
    Delegate d;
    std::unique_ptr<Engine> e;
    explicit Card(const std::vector<std::pair<std::string, bool>>& scripts, EngineOptions o = {}, std::shared_ptr<Resources> r = g_bundle) {
        e = std::make_unique<Engine>(r, o, d);
        e->start(boot_arg(scripts));
    }
    std::string probe(const std::string& code) { return e->probe(code); }
    void event(const std::string& name) { e->post(Engine::Input::Event, Json::object().set("type", name).dump(), 0); }
};

std::vector<std::pair<std::string, bool>> js(std::initializer_list<std::string> list) {
    std::vector<std::pair<std::string, bool>> out;
    for (auto& s : list) out.emplace_back(s, false);
    return out;
}

} // namespace

int main() {
    g_bundle = std::make_shared<Resources>();
    g_bundle->bundle = read_file(std::string(TWCARD_TEST_BUNDLES) + "/test-runtime.js");
    check(!g_bundle->bundle.empty(), "test bundle read");
    check(Resources::header(g_bundle->bundle) == std::make_pair(std::string("0.0.0-test"), 1), "bundle header parsed");
    check(!Resources::header("// twcard 1.0 protocol x\n") && !Resources::header("var x;\n") && !Resources::header("// twcard 1.0\n"),
          "malformed version lines refused");

    const auto stackAt = g_bundle->bundle.find("runtime stack marker");
    if (stackAt == std::string::npos) return 1;
    const auto stackLine = 1 + std::count(g_bundle->bundle.begin(), g_bundle->bundle.begin() + stackAt, '\n');
    const auto bundleFrame = "twcard.js:" + std::to_string(stackLine) + ":";
    const auto checkLines = [&](std::shared_ptr<Resources> bundle, const std::string& label) {
        Card c(js({"\n\nthrow new Error('line check')"}), {}, std::move(bundle));
        check(wait_for([&] { return c.d.isLoaded.load(); }), label + " loads");
        check(c.probe("lastScriptError.stack").find("card-inline-1.js:3:") != std::string::npos &&
                  c.d.errorsJoined().find("line check") != std::string::npos,
              label + " reports the inline script's third line");
        check(c.probe("testRuntimeStack()").find(bundleFrame) != std::string::npos,
              label + " preserves bundle line and column information");
        check(c.probe("testRuntimeStack.toString()").find("runtime stack marker") == std::string::npos,
              label + " omits retained bundle source");
    };
    checkLines(g_bundle, "first card");
    checkLines(g_bundle, "cached card");

    // E1-ish: inline scripts in order, then loaded; a module with an import fails, the next script still runs.
    {
        Card c({{"document.title += 'a'", false},
                {"document.title += 'b'; throw new Error('boom')", false},
                {"import x from 'y'; document.title += 'NO'", true},
                {"document.title += 'm'", true},
                {"document.title += 'c'", false}});
        check(wait_for([&] { return c.d.isLoaded.load(); }), "loaded reported");
        check(c.probe("document.title") == "abmc[loaded]", "scripts one by one in order, then loaded (got " + c.probe("document.title") + ")");
        std::string errs = c.d.errorsJoined();
        check(errs.find("boom") != std::string::npos, "a failing script reports its error");
        check(errs.find("imports are not supported") != std::string::npos || errs.find("could not load module") != std::string::npos,
              "an import fails the module script (" + errs + ")");
        check(c.d.death().empty(), "an error does not kill");
    }
    // Scripts inserted during a task run after it (S1.5).
    {
        Card c(js({"onTestEvent = () => { insertScript(\"document.title += 'later'\"); document.title += 'now'; }"}));
        wait_for([&] { return c.d.isLoaded.load(); });
        c.event("click");
        check(wait_for([&] { return c.probe("document.title").find("later") != std::string::npos; }), "inserted script ran");
        check(c.probe("document.title") == "[loaded]nowlater", "after the inserting task");
    }
    // S1.3a: the script descriptor carries its source name and settlement gets actual JS values.
    {
        Card c({{"throw new Error('named error')", false}, {"export const ready = true", true}});
        check(wait_for([&] { return c.d.isLoaded.load(); }), "named script reports loaded");
        check(c.probe("lastScriptError instanceof Error && lastScriptError.stack.includes('card-inline-1.js') && "
                      "lastScriptResult instanceof Promise") == "true", "script completion sees the thrown error and the module promise");
    }

    // E2: runaway loops die with a cpu: reason however the loop is wrapped.
    {
        const char* variants[] = {
            "onTestEvent = () => { while (true) {} }",
            "onTestEvent = () => { try { while (true) {} } catch (e) { document.title = 'caught'; } }",
            "onTestEvent = () => { new Promise(() => { while (true) {} }).catch(() => { document.title = 'caught'; }); }",
            "onTestEvent = () => { try { throw 1; } finally { while (true) {} } }",
            "onTestEvent = () => { Promise.resolve().then(() => { while (true) {} }).catch(() => {}); }",
            "onTestEvent = () => { async function* g() { try { yield 1; } finally { while (true) {} } } const it = g(); it.next().then(() => it.return()).catch(() => {}); }",
            "onTestEvent = () => { for (;;) { try { while (true) {} } finally { continue; } } }",
        };
        int i = 0;
        for (const char* v : variants) {
            EngineOptions o;
            o.cpuLimitMs = 100;
            Card c(js({v}), o);
            wait_for([&] { return c.d.isLoaded.load(); });
            auto t0 = std::chrono::steady_clock::now();
            c.event("click");
            bool died = wait_for([&] { return c.d.deathCount() > 0; }, 5000);
            check(died && c.d.death().rfind("cpu: one task ran over 100 ms (", 0) == 0 && c.d.deathCount() == 1,
                  "E2 variant " + std::to_string(i) + ": dead with a cpu reason (" + c.d.death() + ", " + std::to_string(int(ms_since(t0))) + " ms)");
            check(c.probe("document.title") == "Error: dead", "E2 variant " + std::to_string(i) + ": no more card code");
            i++;
        }
    }

    // E3: memory.
    {
        EngineOptions o;
        o.memoryLimitBytes = size_t(8) << 20;
        Card c(js({"onTestEvent = () => { const a = []; for (let i = 0; ; i++) a.push('x'.repeat(1000) + i); }"}), o);
        wait_for([&] { return c.d.isLoaded.load(); });
        c.event("click");
        check(wait_for([&] { return c.d.deathCount() > 0; }), "E3: dead");
        check(c.d.death() == "memory: over the 8 MiB limit", "E3: memory reason (" + c.d.death() + ")");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(c.d.deathCount() == 1, "E3: dead once");
    }
    {
        EngineOptions o;
        Card c(js({"onTestEvent = () => { const a = []; try { for (;;) a.push(new Array(10000).fill(1)); } catch (e) { document.title = 'caught ' + e; } }"}), o);
        wait_for([&] { return c.d.isLoaded.load(); });
        c.event("click");
        check(wait_for([&] { return c.d.deathCount() > 0; }) && c.d.death().rfind("memory:", 0) == 0, "E3: out of memory is not catchable");
    }

    // E4: the host functions are gone from the card world.
    {
        Card c(js({}));
        wait_for([&] { return c.d.isLoaded.load(); });
        check(c.probe("typeof host + typeof hostBinary + typeof dispatch") == "undefinedundefinedundefined",
              "E4: no global names");
        check(c.probe("Object.getOwnPropertyNames(globalThis).filter(n => /twcard|host|dispatch/i.test(n)).join()") == "", "E4: not enumerable either");
        std::string leak = c.probe(
            "(() => { let found = []; Error.prepareStackTrace = (e, s) => s; "
            "const st = (function inner() { return new Error().stack; })(); Error.prepareStackTrace = undefined; "
            "if (Array.isArray(st)) for (const cs of st) { if (cs.getFunction() !== undefined || cs.getThis() !== undefined) found.push(cs.getFunctionName()); } "
            "return found.length ? found.join() : 'clean'; })()");
        check(leak == "clean", "E4: CallSites hand out no functions (" + leak + ")");
    }

    // E5: quota. A card burning 80 ms per setTimeout(0) keeps running, at about the quota's rate.
    {
        EngineOptions o;
        Card c(js({"globalThis.n = 0; function burn() { const t = Date.now(); while (Date.now() - t < 80) {} n++; setTimeout(burn, 0); } setTimeout(burn, 0);"
                   "onTestEvent = () => { document.title = 'clicked ' + n; }"}),
               o);
        wait_for([&] { return c.d.isLoaded.load(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        const double js0 = c.e->jsMs();
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        const double perSecond = (c.e->jsMs() - js0) / 3;
        int n = std::atoi(c.probe("n").c_str());
        // The quota is on the JS thread's CPU time: about 300 ms a second, whatever the wall time of each task.
        check(perSecond > 150 && perSecond < 420, "E5: ~300 ms of JS CPU time a second (" + std::to_string(int(perSecond)) + " ms/s, " + std::to_string(n) + " runs)");
        check(c.d.diagCount("cpu-throttled") >= 1 && c.d.diagCount("cpu-throttled") <= 4, "E5: cpu-throttled, at most once a second");
        auto t0 = std::chrono::steady_clock::now();
        c.event("click");
        check(wait_for([&] { return c.probe("document.title").rfind("clicked", 0) == 0; }, 1000), "E5: a click is still answered");
        check(ms_since(t0) < 400, "E5: within one task (" + std::to_string(int(ms_since(t0))) + " ms)");
        check(c.d.death().empty() && c.e->deferredTasks() > 0, "E5: never killed; deferred tasks counted");
    }

    // E6: query limit.
    {
        Card c(js({"onTestEvent = () => { for (let i = 0; i < 200; i++) query({q: 'box', id: i}); document.title = 'survived'; }"}));
        wait_for([&] { return c.d.isLoaded.load(); });
        c.event("click");
        check(wait_for([&] { return c.d.deathCount() > 0; }) && c.d.death() == "layout: too many blocking reads in one task", "E6: 129th query kills");
        check(c.d.queries.load() == 128, "E6: 128 reached the host (" + std::to_string(c.d.queries.load()) + ")");
    }
    {
        Card c(js({"onTestEvent = () => { for (let i = 0; i < 20; i++) { try { query({q: 'box', id: i}); } catch (e) {} } }"}));
        c.d.queryDelayMs = 60;
        wait_for([&] { return c.d.isLoaded.load(); });
        c.event("click");
        check(wait_for([&] { return c.d.deathCount() > 0; }) && c.d.death() == "layout: too many blocking reads in one task", "E6: 1 s of query wall time kills");
    }
    {
        // Query wall time is not the card's CPU time: a slow renderer does not trip the cpu limit.
        EngineOptions o;
        o.cpuLimitMs = 100;
        Card c(js({"onTestEvent = () => { for (let i = 0; i < 9; i++) query({q: 'box', id: i}); document.title = 'ok'; }"}), o);
        c.d.queryDelayMs = 100;
        wait_for([&] { return c.d.isLoaded.load(); });
        c.event("click");
        check(wait_for([&] { return c.probe("document.title") == "ok"; }) && c.d.death().empty(), "blocking reads move the wall-clock backstop");
    }

    // E7: pause.
    {
        Card c(js({"globalThis.ticks = 0; globalThis.frames = 0; function tick() { ticks++; setTimeout(tick, 100); } setTimeout(tick, 100);"
                   "function frame() { frames++; requestAnimationFrame(frame); } requestAnimationFrame(frame);"}));
        wait_for([&] { return c.d.isLoaded.load(); });
        std::atomic<bool> pump{true};
        std::thread renderer([&] {
            int granted = 0;
            while (pump) {
                if (c.d.frames.load() > granted) {
                    granted++;
                    c.e->frame(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count());
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        int framesBefore = std::atoi(c.probe("frames").c_str());
        check(framesBefore >= 5, "frames run while visible (" + std::to_string(framesBefore) + ")");
        c.e->setVisible(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int f0 = std::atoi(c.probe("frames").c_str()), t0 = std::atoi(c.probe("ticks").c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        int f1 = std::atoi(c.probe("frames").c_str()), t1 = std::atoi(c.probe("ticks").c_str());
        check(f1 - f0 == 0, "E7: no frames while hidden (" + std::to_string(f1 - f0) + ")");
        check(t1 - t0 <= 4, "E7: timers at most once a second while hidden (" + std::to_string(t1 - t0) + " in 3 s)");
        check(c.probe("document.hidden") == "true", "E7: the runtime knows");
        c.e->setVisible(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        int f2 = std::atoi(c.probe("frames").c_str());
        check(f2 - f1 >= 5, "E7: frames resume (" + std::to_string(f2 - f1) + ")");
        check(c.probe("document.title") == "[loaded][hidden][shown]", "E7: visibility dispatched twice");
        pump = false;
        renderer.join();
    }

    // E8: bundle mismatch.
    {
        auto other = std::make_shared<Resources>();
        other->bundle = "// twcard 9.9.9 protocol 2\n" + g_bundle->bundle.substr(g_bundle->bundle.find('\n') + 1);
        Card c(js({"document.title = 'ran'"}), {}, other);
        check(wait_for([&] { return c.d.deathCount() > 0; }) && c.d.death() == "runtime bundle mismatch", "E8: dead, runtime bundle mismatch");
        check(c.d.diagCount("bundle-mismatch") == 1 && !c.d.isLoaded, "E8: diagnostic, no card code ran");
    }
    {
        auto broken = std::make_shared<Resources>();
        broken->bundle = "// twcard 0.0.0 protocol 1\nthrow new Error('bad bundle')";
        Card c(js({"document.title = 'ran'"}), {}, broken);
        check(wait_for([&] { return c.d.deathCount() > 0; }) && c.d.death() == "runtime bundle failed to load", "a failing bundle: fail closed");
    }
    {
        auto broken = std::make_shared<Resources>();
        broken->bundle = "// twcard 0.0.0 protocol 1\nconst = 1;";
        Card c(js({"document.title = 'ran'"}), {}, broken);
        check(wait_for([&] { return c.d.deathCount() > 0; }) && c.d.death() == "runtime bundle failed to load" &&
                  c.d.diagCount("bundle") == 1 && !c.d.isLoaded, "a compiler failure reports a bundle diagnostic and fails closed");
    }
    {
        auto withBc = std::make_shared<Resources>(*g_bundle);
        std::string bad = "twcard-bytecode // twcard 0.0.1 protocol 1\n";
        withBc->bytecode.assign(bad.begin(), bad.end());
        withBc->bytecode.push_back(1);
        Card c(js({"document.title += 'x'"}), {}, withBc);
        check(wait_for([&] { return c.d.isLoaded.load(); }) && c.probe("document.title") == "x[loaded]" && c.d.diagCount("bytecode") == 1,
              "bytecode of another version: ignored, source evaluated, diagnostic");
    }

#ifdef TWCARD_TEST_BYTECODE
    {
        auto withBc = std::make_shared<Resources>(*g_bundle);
        std::string bc = read_file(TWCARD_TEST_BYTECODE);
        withBc->bytecode.assign(bc.begin(), bc.end());
        Card c(js({"document.title += 'bc'"}), {}, withBc);
        check(wait_for([&] { return c.d.isLoaded.load(); }) && c.probe("document.title") == "bc[loaded]" && c.d.diagCount("bytecode") == 0,
              "the bundle's own bytecode (twcard_bytecode) is used");
        checkLines(withBc, "precompiled bundle");
    }
#endif
    // Timers: order, cap (S1.3: refused beyond maxTimers), clearTimer removes a pending fire.
    {
        EngineOptions o;
        o.maxTimers = 4;
        Card c(js({"globalThis.order = ''; setTimeout(() => order += 'b', 20); setTimeout(() => order += 'a', 0);"
                   "for (let i = 0; i < 5; i++) setTimeout(() => order += 'x', 50);"
                   "const id = setTimeout(() => order += 'CLEARED', 10); clearTimeout(id);"}),
               o);
        wait_for([&] { return c.d.isLoaded.load(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::string order = c.probe("order");
        check(order == "abxx", "timers in due order, beyond the cap refused (" + order + ")");
        check(c.d.diagCount("timer-limit-enforced") == 4, "timer-limit-enforced per refused timer (" + std::to_string(c.d.diagCount("timer-limit-enforced")) + ")");
    }
    // now: monotonic, 0.1 ms resolution; random.
    {
        Card c(js({}));
        wait_for([&] { return c.d.isLoaded.load(); });
        std::string r = c.probe("(() => { const a = performance.now(), b = performance.now(); return (b >= a) + ' ' + (Math.round(a * 10) === a * 10); })()");
        check(r == "true true", "now: monotonic, coarsened to 0.1 ms (" + r + ")");
        check(c.probe("(() => { const b = new Uint8Array(randomBytes(64)); return b.length + ' ' + b.some(x => x !== 0); })()") == "64 true", "random bytes");
        check(c.probe("randomBytes(65537)") == "null", "random: at most 65536 bytes");
    }
    // Probe: String(value), errors as Error: message.
    {
        Card c(js({"globalThis.x = {toString() { return 'X!'; }}"}));
        wait_for([&] { return c.d.isLoaded.load(); });
        check(c.probe("x") == "X!" && c.probe("1+1") == "2" && c.probe("nope()") == "Error: nope is not defined", "probe results");
    }
    // Stop while a task runs and while a query blocks: prompt.
    {
        Card c(js({"onTestEvent = () => { const t = Date.now(); while (Date.now() - t < 400) {} }"}));
        wait_for([&] { return c.d.isLoaded.load(); });
        c.event("click");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto t0 = std::chrono::steady_clock::now();
        c.e->stop();
        check(ms_since(t0) < 200 && c.d.deathCount() == 0, "H5: stop during a 400 ms task returns promptly, no dead report");
    }
    {
        Card c(js({"onTestEvent = () => { query({q:'box', id: 1}); }"}));
        c.d.queryDelayMs = 2500;
        wait_for([&] { return c.d.isLoaded.load(); });
        c.event("click");
        wait_for([&] { return c.d.queries.load() > 0; });
        auto t0 = std::chrono::steady_clock::now();
        c.e->stop();
        check(ms_since(t0) < 200, "H5: stop during a blocked query returns promptly (" + std::to_string(int(ms_since(t0))) + " ms)");
    }
    // Kill from the host (a validator cap).
    {
        Card c(js({}));
        wait_for([&] { return c.d.isLoaded.load(); });
        c.e->kill("nodes: 2001 (max 2000)");
        check(wait_for([&] { return c.d.deathCount() > 0; }) && c.d.death() == "nodes: 2001 (max 2000)", "kill() reports dead with its reason");
        c.e->kill("again");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(c.d.deathCount() == 1, "once");
    }
    // Unhandled rejections are reported.
    {
        Card c(js({"onTestEvent = () => { reason = new Error('nobody'); promise = Promise.reject(reason); "
                   "Promise.reject(1).catch(() => {}); }"}));
        wait_for([&] { return c.d.isLoaded.load(); });
        c.event("click");
        wait_for([&] { return !c.d.errorsJoined().empty(); });
        std::string errs = c.d.errorsJoined();
        check(errs.find("nobody") != std::string::npos && errs.find("unhandled rejection: 1") == std::string::npos, "unhandled rejections: only the unhandled one (" + errs + ")");
        check(c.probe("lastRejectedPromise === promise && lastRejectionReason === reason") == "true",
              "unhandled rejection dispatch preserves the original JS values");
    }

    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
