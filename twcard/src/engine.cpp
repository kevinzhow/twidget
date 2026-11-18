// twcard::Engine: SPEC S1. See engine.h for the shape; this file is the QuickJS side and the task loop.
#include "twcard/engine.h"

#include "bundle_bytecode.h"
#include "engine_protocol.h"
#include "twidget/json.h"

#include <quickjs.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <pthread.h>
#include <sstream>
#include <stdexcept>

#if defined(__APPLE__)
#include <stdlib.h>  // arc4random_buf
#else
#include <sys/random.h>
#endif

namespace twcard {

namespace {

using twidget::Json;

double steady_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// The calling thread's CPU time, ms (S1.4.2: the deadline is on the JS thread's CPU time).
double thread_cpu_ms() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return double(ts.tv_sec) * 1000.0 + double(ts.tv_nsec) / 1e6;
}

/// S1.3 `random`: CSPRNG bytes from the OS.
bool random_bytes(uint8_t* out, size_t n) {
#if defined(__APPLE__)
    arc4random_buf(out, n);
    return true;
#else
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(out + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        got += size_t(r);
    }
    return true;
#endif
}

constexpr size_t kStackBytes = size_t(8) << 20;             // S1.1: a large stack, 8 MB
constexpr size_t kQuickJsStackBytes = kStackBytes - (size_t(512) << 10);  // QuickJS's own limit, just below it
constexpr int kMaxQueriesPerTask = 128;                    // S1.7
constexpr double kMaxQueryWallMsPerTask = 1000;            // S1.7
constexpr double kMaxTimerDelayMs = 24.0 * 3600 * 1000;    // S4.1: delays clamped to [0, 24 h]
constexpr double kHiddenTimerMs = 1000;                    // S1.8
constexpr size_t kMaxRandomBytes = 65536;                  // S1.3
constexpr const char* kLayoutReason = "layout: too many blocking reads in one task";

std::string memory_reason(size_t limit) {
    return "memory: over the " + Json::number(double(limit) / double(1 << 20)) + " MiB limit";
}

// SPEC S0: compiled trusted bundles are shared as byte strings across QuickJS runtimes.
struct BundleCache {
    std::mutex mutex;
    std::map<std::string, CompiledBundle, std::less<>> entries;
};

const CompiledBundle& cachedBundle(const std::string& source) {
    static BundleCache cache;
    std::lock_guard lock(cache.mutex);
    if (auto entry = cache.entries.find(source); entry != cache.entries.end()) return entry->second;
    auto compiled = compileTrustedBundle(source);
    // Entries are immutable and never erased. Card threads deserialize them without holding the cache mutex.
    return cache.entries.emplace(source, std::move(compiled)).first->second;
}

} // namespace

// ───────────────────────────── Resources ─────────────────────────────

std::optional<std::pair<std::string, int>> Resources::header(std::string_view bundle) {
    // `// twcard <semver> protocol <n>`
    std::string_view line = bundle.substr(0, bundle.find('\n'));
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    constexpr std::string_view prefix = "// twcard ";
    if (line.substr(0, prefix.size()) != prefix) return std::nullopt;
    line.remove_prefix(prefix.size());
    size_t space = line.find(' ');
    if (space == std::string_view::npos || space == 0) return std::nullopt;
    std::string semver(line.substr(0, space));
    line.remove_prefix(space);
    constexpr std::string_view proto = " protocol ";
    if (line.substr(0, proto.size()) != proto) return std::nullopt;
    line.remove_prefix(proto.size());
    if (line.empty() || line.size() > 6) return std::nullopt;
    int n = 0;
    for (char c : line) {
        if (c < '0' || c > '9') return std::nullopt;
        n = n * 10 + (c - '0');
    }
    return std::make_pair(semver, n);
}

std::shared_ptr<Resources> Resources::fromDirectory(const std::string& dir) {
    auto read = [](const std::string& path, bool required) -> std::optional<std::string> {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            if (required) throw std::runtime_error("cannot read " + path);
            return std::nullopt;
        }
        std::ostringstream s;
        s << f.rdbuf();
        return s.str();
    };
    auto r = std::make_shared<Resources>();
    r->bundle = *read(dir + "/twcard.js", true);
    if (auto bc = read(dir + "/twcard.qbc", false)) r->bytecode.assign(bc->begin(), bc->end());
    return r;
}

// ───────────────────────────── Impl (QuickJS) ─────────────────────────────

struct Engine::Impl {
    Engine& e;
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    JSValue dispatch = JS_UNDEFINED;
    pthread_t thread{};

    // The running task's limits (S1.4.2, S1.7).
    double cpuStart = 0, cpuDeadline = 0, wallDeadline = 0;
    std::string taskKind;
    std::string interruptReason;  // set by the interrupt handler when it stopped the task
    int queries = 0;
    double queryWallMs = 0;
    bool uncaught = false;
    double lastMemoryAt = -1e18;

    // Unhandled promise rejections seen during the task (S1.4.7), until a handler comes.
    std::vector<std::pair<JSValue, JSValue>> rejections;

    explicit Impl(Engine& engine) : e(engine) {}

    // ── setup ──

    static int interrupt(JSContext*, void* opaque) {
        auto* self = static_cast<Impl*>(opaque);
        if (self->e.killRequested_.load()) return 1;
        if (!self->interruptReason.empty()) return 1;
        if (self->cpuDeadline <= 0) return 0;
        if (thread_cpu_ms() > self->cpuDeadline || steady_ms() > self->wallDeadline) {
            self->interruptReason = "cpu: one task ran over " + Json::number(self->e.options_.cpuLimitMs) + " ms (" + self->taskKind + ")";
            return 1;
        }
        return 0;
    }

    static JSModuleDef* refuse_module(JSContext* ctx, const char* name, void*) {
        // S1.5: no import resolves.
        JS_ThrowReferenceError(ctx, "could not load module '%s': imports are not supported", name);
        return nullptr;
    }

    static void track_rejection(JSContext* ctx, JSValueConst promise, JSValueConst reason, bool handled, void* opaque) {
        auto* self = static_cast<Impl*>(opaque);
        if (handled) {
            for (auto it = self->rejections.begin(); it != self->rejections.end(); ++it)
                if (JS_VALUE_GET_PTR(it->first) == JS_VALUE_GET_PTR(promise)) {
                    JS_FreeValue(ctx, it->first);
                    JS_FreeValue(ctx, it->second);
                    self->rejections.erase(it);
                    return;
                }
            return;
        }
        self->rejections.emplace_back(JS_DupValue(ctx, promise), JS_DupValue(ctx, reason));
    }

    bool create() {
        rt = JS_NewRuntime();
        if (!rt) return false;
        JS_SetRuntimeOpaque(rt, this);
        JS_SetMemoryLimit(rt, e.options_.memoryLimitBytes);
        JS_SetMaxStackSize(rt, kQuickJsStackBytes);
        JS_SetInterruptHandler(rt, interrupt, this);
        JS_SetModuleLoaderFunc(rt, nullptr, refuse_module, nullptr);
        JS_SetHostPromiseRejectionTracker(rt, track_rejection, this);
        ctx = JS_NewContext(rt);
        if (!ctx) return false;
        JS_SetContextOpaque(ctx, this);
        return true;
    }

    void destroy() {
        for (auto& [promise, reason] : rejections) {
            JS_FreeValue(ctx, promise);
            JS_FreeValue(ctx, reason);
        }
        rejections.clear();
        if (ctx) {
            JS_FreeValue(ctx, dispatch);
            dispatch = JS_UNDEFINED;
            JS_FreeContext(ctx);
            ctx = nullptr;
        }
        if (rt) {
            JS_FreeRuntime(rt);
            rt = nullptr;
        }
    }

    // ── host functions (S1.3) ──

    static JSValue host(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
        auto* self = static_cast<Impl*>(JS_GetContextOpaque(ctx));
        return self->host_call(argc, argv);
    }

    static JSValue host_binary(JSContext* ctx, JSValueConst, int, JSValueConst*) {
        auto* self = static_cast<Impl*>(JS_GetContextOpaque(ctx));
        auto bin = self->e.delegate_.hostBinary();
        if (!bin) return JS_NULL;
        return JS_NewArrayBufferCopy(ctx, bin->data(), bin->size());
    }

    JSValue to_js(const HostValue& v) {
        switch (v.kind) {
        case HostValue::Kind::Undefined: return JS_UNDEFINED;
        case HostValue::Kind::Null: return JS_NULL;
        case HostValue::Kind::String: return JS_NewStringLen(ctx, v.text.data(), v.text.size());
        case HostValue::Kind::Number: return JS_NewFloat64(ctx, v.number);
        }
        return JS_UNDEFINED;
    }

    /// Stops the running task at once: the card is being killed (an uncatchable error, S1.4.2).
    JSValue stop_now() { return JS_ThrowInternalError(ctx, "card stopped"); }

    JSValue host_call(int argc, JSValueConst* argv) {
        if (e.killRequested_.load() || e.dead_.load()) return stop_now();
        if (argc < 1 || !JS_IsString(argv[0])) return JS_UNDEFINED;
        size_t klen = 0;
        const char* kc = JS_ToCStringLen(ctx, &klen, argv[0]);
        if (!kc) return JS_EXCEPTION;
        std::string kind(kc, klen);
        JS_FreeCString(ctx, kc);

        std::string arg;
        bool hasArg = false;
        double numArg = 0;
        bool isNum = false;
        if (argc >= 2 && JS_IsString(argv[1])) {
            // The cap is checked on the string's length before converting it (S1.3).
            JSValue lenv = JS_GetPropertyStr(ctx, argv[1], "length");
            int64_t units = 0;
            JS_ToInt64(ctx, &units, lenv);
            JS_FreeValue(ctx, lenv);
            if (size_t(units) > protocol::argument_cap(kind)) {
                e.delegate_.diagnostic("oversize", kind + ": " + std::to_string(units));
                return JS_UNDEFINED;
            }
            size_t len = 0;
            const char* a = JS_ToCStringLen(ctx, &len, argv[1]);
            if (!a) return JS_EXCEPTION;
            arg.assign(a, len);
            JS_FreeCString(ctx, a);
            hasArg = true;
            if (arg.size() > protocol::argument_cap(kind)) {
                e.delegate_.diagnostic("oversize", kind + ": " + std::to_string(arg.size()));
                return JS_UNDEFINED;
            }
        } else if (argc >= 2 && JS_IsNumber(argv[1])) {
            JS_ToFloat64(ctx, &numArg, argv[1]);
            isNum = true;
            arg = Json::number(numArg);
            hasArg = true;
        }
        (void)hasArg;

        // Kinds the engine answers itself.
        if (kind == "now") return JS_NewFloat64(ctx, std::floor(e.now() * 10) / 10);  // 0.1 ms resolution
        if (kind == "timer") {
            arm_timer(arg);
            return JS_UNDEFINED;
        }
        if (kind == "clearTimer") {
            int64_t id = isNum ? int64_t(numArg) : Json::parse(arg).value_or(Json()).asInt64(-1);
            clear_timer(id);
            return JS_UNDEFINED;
        }
        if (kind == "raf") {
            want_frame();
            return JS_UNDEFINED;
        }
        if (kind == "random") {
            double n = isNum ? numArg : Json::parse(arg).value_or(Json()).asNumber(-1);
            if (!(n >= 0 && n <= double(kMaxRandomBytes))) return JS_NULL;
            std::vector<uint8_t> bytes(static_cast<size_t>(n));
            if (!bytes.empty() && !random_bytes(bytes.data(), bytes.size())) return JS_NULL;
            return JS_NewArrayBufferCopy(ctx, bytes.data(), bytes.size());
        }
        if (kind == "error") {
            if (arg.size() > protocol::kMaxErrorBytes) {
                size_t count = protocol::kMaxErrorBytes;
                while ((static_cast<unsigned char>(arg[count]) & 0xc0) == 0x80) --count;
                arg.resize(count);
            }
            report_uncaught(arg);
            return JS_UNDEFINED;
        }

        // The rest goes to the host.
        const bool query = kind == "query";
        if (query && ++queries > kMaxQueriesPerTask) {
            e.kill(kLayoutReason);
            return stop_now();
        }
        double mainMs = 0;
        const double t0 = steady_ms();
        HostValue v = e.delegate_.hostCall(kind, arg, mainMs);
        const double waited = steady_ms() - t0;
        if (query) {
            // A blocking read is the renderer's time, not the card's: it moves the wall-clock backstop.
            wallDeadline += waited;
            queryWallMs += waited;
            if (queryWallMs > kMaxQueryWallMsPerTask) e.kill(kLayoutReason);
        }
        if (mainMs > 0) e.throttle_.chargeMain(e.now(), mainMs);
        if (e.killRequested_.load()) return stop_now();
        return to_js(v);
    }

    // ── timers and frames (S4.1, S4.2, S1.8) ──

    void arm_timer(const std::string& arg) {
        auto j = Json::parse(arg);
        if (!j) return;
        int64_t id = (*j)["id"].asInt64(0);
        double delay = (*j)["delay"].asNumber(0);
        if (id <= 0) return;
        if (!std::isfinite(delay) || delay < 0) delay = 0;
        delay = std::min(delay, kMaxTimerDelayMs);
        if (!e.visible_) delay = std::max(delay, kHiddenTimerMs);
        if (!e.timers_.count(id) && int(e.timers_.size()) >= e.options_.maxTimers) {
            e.delegate_.diagnostic("timer-limit-enforced", std::to_string(id));
            return;
        }
        e.timers_[id] = {e.now() + delay};
    }

    void clear_timer(int64_t id) {
        e.timers_.erase(id);
        if (e.pendingFires_.erase(id)) {
            std::lock_guard l(e.mutex_);
            for (auto it = e.queue_.begin(); it != e.queue_.end(); ++it)
                if (it->kind == Kind::Timer && it->timerId == id) {
                    e.queue_.erase(it);
                    break;
                }
        }
    }

    void want_frame() {
        if (!e.visible_) {
            e.frameWanted_ = true;  // issued on return (S1.8)
            return;
        }
        if (e.frameRequested_) return;
        e.frameRequested_ = true;
        e.delegate_.requestFrame();
    }

    // ── calling the runtime ──

    std::string describe(JSValueConst v) {
        std::string out;
        const char* s = JS_ToCString(ctx, v);
        if (s) {
            out = s;
            JS_FreeCString(ctx, s);
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
            out = "(unprintable)";
        }
        if (JS_IsObject(v)) {
            JSValue stack = JS_GetPropertyStr(ctx, v, "stack");
            if (JS_IsString(stack)) {
                const char* st = JS_ToCString(ctx, stack);
                if (st && *st) {
                    out += "\n";
                    out += st;
                }
                if (st) JS_FreeCString(ctx, st);
            } else if (JS_IsException(stack)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
            }
            JS_FreeValue(ctx, stack);
        }
        if (out.size() > 4096) out.resize(4096);
        return out;
    }

    bool fatalException(JSValueConst exc) {
        std::string reason;
        if (e.killRequested_.load()) {
            std::lock_guard l(e.mutex_);
            reason = e.killReason_.empty() ? "stopped" : e.killReason_;
        } else if (!interruptReason.empty()) {
            reason = interruptReason;
        } else if (JS_IsUncatchableError(exc)) {
            // Leave room to inspect the exception when QuickJS reached the runtime's memory limit.
            JS_SetMemoryLimit(rt, e.options_.memoryLimitBytes + (size_t(1) << 20));
            const auto text = describe(exc);
            JS_SetMemoryLimit(rt, e.options_.memoryLimitBytes);
            if (text.find("out of memory") != std::string::npos) reason = memory_reason(e.options_.memoryLimitBytes);
        }
        if (reason.empty()) return false;
        e.die(reason);
        return true;
    }

    void on_exception(bool dispatchFailure = false) {
        JSValue exc = JS_GetException(ctx);
        if (!fatalException(exc)) {
            const auto text = describe(exc);
            report_uncaught(text);
            if (dispatchFailure) e.delegate_.diagnostic("protocol", text);
        }
        JS_FreeValue(ctx, exc);
    }

    void report_uncaught(const std::string& text) {
        uncaught = true;
        e.delegate_.uncaughtError(text);
    }

    /// S1.3a: the caller owns the return value. JS arguments are borrowed.
    JSValue callValue(const char* kind, JSValueConst arg = JS_UNDEFINED, JSValueConst arg2 = JS_UNDEFINED) {
        if (e.dead_.load() || e.killRequested_.load()) return JS_UNDEFINED;
        JSValue args[] = {JS_NewString(ctx, kind), arg, arg2};
        JSValue result = JS_IsException(args[0]) ? JS_EXCEPTION : JS_Call(ctx, dispatch, JS_UNDEFINED, 3, args);
        JS_FreeValue(ctx, args[0]);
        if (JS_IsException(result)) {
            on_exception(true);
            return JS_UNDEFINED;
        }
        return result;
    }

    void call(const char* kind) { JS_FreeValue(ctx, callValue(kind)); }

    void call(const char* kind, std::string_view arg) {
        JSValue text = JS_NewStringLen(ctx, arg.data(), arg.size());
        if (JS_IsException(text)) {
            on_exception();
            return;
        }
        JS_FreeValue(ctx, callValue(kind, text));
        JS_FreeValue(ctx, text);
    }

    void drain_jobs() {
        while (!e.dead_.load() && !e.killRequested_.load() && JS_IsJobPending(rt)) {
            if (interrupt(ctx, this)) {
                if (!interruptReason.empty()) e.die(interruptReason);
                return;
            }
            JSContext* c = nullptr;
            const int result = JS_ExecutePendingJob(rt, &c);
            if (result < 0) on_exception();
        }
    }

    void report_rejections() {
        auto list = std::move(rejections);
        rejections.clear();
        for (auto& [promise, reason] : list) {
            JS_FreeValue(ctx, callValue(protocol::kRejection, promise, reason));
            JS_FreeValue(ctx, reason);
            JS_FreeValue(ctx, promise);
        }
    }

    /// S1.4.4 and 4a: settle normal work, dispatch unhandled rejections in order, then settle their work.
    void settle() {
        for (int round = 0; round < protocol::kMaxSettleRounds; ++round) {
            drain_jobs();
            if (e.dead_.load() || e.killRequested_.load()) return;
            JSValue result = callValue(protocol::kEndTick);
            double code = 0;
            if (JS_IsNumber(result)) JS_ToFloat64(ctx, &code, result);
            JS_FreeValue(ctx, result);
            if (e.dead_.load() || e.killRequested_.load()) return;
            if (code == 1 || JS_IsJobPending(rt)) continue;
            if (rejections.empty()) return;
            report_rejections();
        }
        e.delegate_.diagnostic("settle-limit", taskKind);
    }

    bool eval_bundle() {
        const Resources& res = *e.resources_;
        std::string_view first = std::string_view(res.bundle).substr(0, res.bundle.find('\n'));
        JSValue r = JS_UNDEFINED;
        bool done = false;
        if (!res.bytecode.empty()) {
            const std::string expect = "twcard-bytecode " + std::string(first) + "\n";
            if (res.bytecode.size() > expect.size() && std::memcmp(res.bytecode.data(), expect.data(), expect.size()) == 0) {
                // Our own build's output, never card input (QuickJS reads bytecode only from trusted sources).
                JSValue fn = JS_ReadObject(ctx, res.bytecode.data() + expect.size(), res.bytecode.size() - expect.size(), JS_READ_OBJ_BYTECODE);
                if (!JS_IsException(fn)) {
                    r = JS_EvalFunction(ctx, fn);
                    done = true;
                } else {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    e.delegate_.diagnostic("bytecode", "unreadable");
                }
            } else {
                e.delegate_.diagnostic("bytecode", "version differs from the bundle");
            }
        }
        if (!done) {
            const auto& compiled = cachedBundle(res.bundle);
            if (compiled.bytes.empty()) {
                e.delegate_.diagnostic("bundle", compiled.error);
                return false;
            }
            JSValue fn = JS_ReadObject(ctx, compiled.bytes.data(), compiled.bytes.size(), JS_READ_OBJ_BYTECODE);
            r = JS_IsException(fn) ? fn : JS_EvalFunction(ctx, fn);
        }
        if (JS_IsException(r)) {
            JSValue exc = JS_GetException(ctx);
            e.delegate_.diagnostic("bundle", describe(exc));
            JS_FreeValue(ctx, exc);
            return false;
        }
        JS_FreeValue(ctx, r);
        drain_jobs();
        return true;
    }

    /// S1.2.1-2: the bundle with the host functions, then the names gone.
    bool load_bundle() {
        auto hdr = Resources::header(e.resources_->bundle);
        if (!hdr || hdr->second != kProtocol) {
            e.delegate_.diagnostic("bundle-mismatch", hdr ? "protocol " + std::to_string(hdr->second) : "no version line");
            e.die("runtime bundle mismatch");
            return false;
        }
        JSValue global = JS_GetGlobalObject(ctx);
        // Anonymous functions: neither name nor toString points at what they are (S1.2.2).
        const int privateGlobal = JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE;
        bool ok = JS_DefinePropertyValueStr(ctx, global, protocol::kHostGlobal,
                                          JS_NewCFunction(ctx, host, "", 2), privateGlobal) > 0 &&
                  JS_DefinePropertyValueStr(ctx, global, protocol::kHostBinaryGlobal,
                                          JS_NewCFunction(ctx, host_binary, "", 0), privateGlobal) > 0 &&
                  eval_bundle();
        if (ok) {
            dispatch = JS_GetPropertyStr(ctx, global, protocol::kDispatchGlobal);
            ok = JS_IsFunction(ctx, dispatch);
        }
        for (const char* name : {protocol::kHostGlobal, protocol::kHostBinaryGlobal, protocol::kDispatchGlobal}) {
            JSAtom atom = JS_NewAtom(ctx, name);
            if (JS_DeleteProperty(ctx, global, atom, 0) <= 0) ok = false;
            JS_FreeAtom(ctx, atom);
        }
        JS_FreeValue(ctx, global);
        if (!ok) {
            e.die("runtime bundle failed to load");
            return false;
        }
        return true;
    }

    /// S1.3a and S1.5: one script descriptor, with the evaluation result or thrown value returned to the runtime.
    bool run_script() {
        JSValue descriptor = callValue(protocol::kScript);
        if (!JS_IsString(descriptor)) {
            JS_FreeValue(ctx, descriptor);
            return false;
        }
        size_t size = 0;
        const char* bytes = JS_ToCStringLen(ctx, &size, descriptor);
        JS_FreeValue(ctx, descriptor);
        if (!bytes) {
            on_exception();
            return false;
        }
        const auto script = Json::parse(std::string_view(bytes, size));
        JS_FreeCString(ctx, bytes);
        if (!script || !script->isObject()) {
            e.delegate_.diagnostic("protocol", "invalid script descriptor");
            return false;
        }
        if (script->empty()) return true;  // S1.3a: the runtime handled a skipped script.
        const auto& name = (*script)["name"];
        const auto& text = (*script)["text"];
        const auto& module = (*script)["module"];
        if (!name.isString() || !text.isString() || !module.isBool()) {
            e.delegate_.diagnostic("protocol", "invalid script fields");
            return false;
        }
        const auto& source = text.asString();
        const int mode = module.asBool() ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
        JSValue result = JS_Eval(ctx, source.data(), source.size(), name.asString().c_str(), mode);
        if (JS_IsException(result)) {
            JSValue exc = JS_GetException(ctx);
            if (!fatalException(exc)) {
                if (JS_IsUncatchableError(exc)) report_uncaught(describe(exc));
                else JS_FreeValue(ctx, callValue(protocol::kScriptError, exc));
            }
            JS_FreeValue(ctx, exc);
        } else {
            JS_FreeValue(ctx, callValue(protocol::kScriptDone, result));
            JS_FreeValue(ctx, result);
        }
        return true;
    }

    std::string probe(const std::string& code) {
        JSValue r = JS_Eval(ctx, code.data(), code.size(), "<probe>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) {
            JSValue exc = JS_GetException(ctx);
            if (fatalException(exc)) {
                JS_FreeValue(ctx, exc);
                return "Error: dead";
            }
            std::string out;
            JSValue msg = JS_IsObject(exc) ? JS_GetPropertyStr(ctx, exc, "message") : JS_DupValue(ctx, exc);
            const char* s = JS_ToCString(ctx, msg);
            out = std::string("Error: ") + (s ? s : "");
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, msg);
            JS_FreeValue(ctx, exc);
            return out;
        }
        const char* s = JS_ToCString(ctx, r);
        std::string out = s ? s : "Error: unprintable";
        if (s) JS_FreeCString(ctx, s);
        else JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, r);
        return out;
    }

    void update_memory() {
        double now = steady_ms();
        if (now - lastMemoryAt < 1000) return;
        lastMemoryAt = now;
        JSMemoryUsage u{};
        JS_ComputeMemoryUsage(rt, &u);
        e.memory_ = size_t(u.malloc_size);
    }
};

// ───────────────────────────── Engine ─────────────────────────────

const char* Engine::kindName(Kind k) {
    switch (k) {
    case Kind::Boot: return "boot";
    case Kind::Script: return "script";
    case Kind::Loaded: return "loaded";
    case Kind::Timer: return "timer";
    case Kind::Frame: return "frame";
    case Kind::Event: return "event";
    case Kind::Viewport: return "viewport";
    case Kind::Resize: return "resize";
    case Kind::Theme: return "theme";
    case Kind::SafeArea: return "safeArea";
    case Kind::Bridge: return "bridge";
    case Kind::Visibility: return "visibility";
    case Kind::Probe: return "probe";
    }
    return "task";
}

Engine::Engine(std::shared_ptr<const Resources> resources, EngineOptions options, EngineDelegate& delegate)
    : resources_(std::move(resources)), options_(options), delegate_(delegate), impl_(std::make_unique<Impl>(*this)), throttle_(options.throttle),
      visible_(options.visible) {}

Engine::~Engine() { stop(); }

double Engine::now() const { return steady_ms() - bootSteadyMs_; }

void Engine::start(std::string bootArg) {
    std::lock_guard l(mutex_);
    if (started_ || stopping_) return;
    started_ = true;
    bootArg_ = std::move(bootArg);
    bootSteadyMs_ = steady_ms();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kStackBytes);
    int rc = pthread_create(
        &impl_->thread, &attr,
        [](void* self) -> void* {
            static_cast<Engine*>(self)->threadMain();
            return nullptr;
        },
        this);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        started_ = false;
        joined_ = true;
        throw std::runtime_error("twcard: cannot start the card's thread");
    }
}

void Engine::enqueue(Task t) {
    // Under mutex_. Frames collapse into one pending frame (S1.7).
    if (t.kind == Kind::Frame)
        for (Task& q : queue_)
            if (q.kind == Kind::Frame) {
                q.arg = std::move(t.arg);
                return;
            }
    queue_.push_back(std::move(t));
}

void Engine::post(Input input, std::string arg, int64_t gesture) {
    Kind k = Kind::Event;
    switch (input) {
    case Input::Event: k = Kind::Event; break;
    case Input::Frame: k = Kind::Frame; break;
    case Input::Viewport: k = Kind::Viewport; break;
    case Input::Resize: k = Kind::Resize; break;
    case Input::Theme: k = Kind::Theme; break;
    case Input::SafeArea: k = Kind::SafeArea; break;
    case Input::Bridge: k = Kind::Bridge; break;
    }
    {
        std::lock_guard l(mutex_);
        if (stopping_ || dead_.load()) return;
        Task t{k, std::move(arg)};
        t.gesture = k == Kind::Event ? gesture : 0;
        enqueue(std::move(t));
    }
    cv_.notify_all();
}

void Engine::frame(double steadyMs) {
    post(Input::Frame, Json::object().set("time", std::floor(toCardTime(steadyMs) * 10) / 10).dump());
}

void Engine::setVisible(bool visible) {
    {
        std::lock_guard l(mutex_);
        if (stopping_ || dead_.load()) return;
        queue_.push_back(Task(Kind::Visibility, visible ? "1" : "0"));
    }
    cv_.notify_all();
}

std::string Engine::probe(const std::string& code) {
    auto p = std::make_shared<std::promise<std::string>>();
    auto f = p->get_future();
    {
        std::lock_guard l(mutex_);
        if (!started_ || stopping_) return "Error: not running";
        if (dead_.load()) return "Error: dead";
        Task t{Kind::Probe, code};
        t.probe = p;
        queue_.push_back(std::move(t));
    }
    cv_.notify_all();
    return f.get();
}

void Engine::kill(const std::string& reason) {
    {
        std::lock_guard l(mutex_);
        if (killRequested_.load() || dead_.load()) return;
        killReason_ = reason;
        killRequested_ = true;
    }
    cv_.notify_all();
    delegate_.abortBlocking();
}

void Engine::stop() {
    {
        std::unique_lock l(mutex_);
        if (joined_) return;
        if (stopping_) {
            cv_.wait(l, [this] { return joined_; });
            return;
        }
        stopping_ = true;
        if (!started_) {
            joined_ = true;
            return;
        }
        if (!killRequested_.load()) {
            killReason_.clear();
            killRequested_ = true;  // interrupts the running task
        }
    }
    cv_.notify_all();
    delegate_.abortBlocking();
    pthread_join(impl_->thread, nullptr);
    {
        std::lock_guard l(mutex_);
        joined_ = true;
    }
    cv_.notify_all();
}

void Engine::die(const std::string& reason) {
    // Engine thread (S1.9).
    if (dead_.exchange(true)) return;
    timers_.clear();
    pendingFires_.clear();
    std::deque<Task> dropped;
    bool stopping;
    {
        std::lock_guard l(mutex_);
        dropped.swap(queue_);
        stopping = stopping_;
    }
    for (Task& t : dropped)
        if (t.probe) t.probe->set_value("Error: dead");
    delegate_.abortBlocking();
    if (!stopping) delegate_.dead(reason);
}

void Engine::threadMain() {
    pthread_setname_np(
#if defined(__APPLE__)
        "twcard-js"
#else
        pthread_self(), "twcard-js"
#endif
    );
    if (!impl_->create()) {
        die(memory_reason(options_.memoryLimitBytes));
    } else if (impl_->load_bundle()) {
        loop();
    }
    // Leave: answer whoever still waits, free the runtime.
    {
        std::deque<Task> left;
        {
            std::lock_guard l(mutex_);
            left.swap(queue_);
        }
        for (Task& t : left)
            if (t.probe) t.probe->set_value("Error: dead");
    }
    impl_->destroy();
}

void Engine::fireDueTimers(double now) {
    // Under mutex_ (enqueues), engine thread (timers_).
    if (!visible_ && now < lastHiddenFire_ + kHiddenTimerMs) return;  // S1.8: one shared wake-up a second
    bool any = false;
    for (auto it = timers_.begin(); it != timers_.end();) {
        if (it->second.due > now) {
            ++it;
            continue;
        }
        const int64_t id = it->first;
        it = timers_.erase(it);
        if (pendingFires_.insert(id).second) {  // one pending fire per timer id
            Task t{Kind::Timer, std::to_string(id)};
            t.timerId = id;
            queue_.push_back(std::move(t));
        }
        any = true;
    }
    if (any && !visible_) lastHiddenFire_ = now;
}

void Engine::runQueuedScripts() {
    // S1.4.5 / S1.5: scripts the task inserted run after it, one task each.
    while (!dead_.load() && !killRequested_.load()) {
        Task t{Kind::Script};
        runTask(t);
        if (t.arg != "ran") break;
    }
}

void Engine::runTask(Task& t) {
    Impl& im = *impl_;
    im.taskKind = kindName(t.kind);
    im.interruptReason.clear();
    im.queries = 0;
    im.queryWallMs = 0;
    im.uncaught = false;
    currentGesture_ = t.gesture;
    inEvent_ = t.kind == Kind::Event;
    const double wall0 = steady_ms();
    im.cpuStart = thread_cpu_ms();
    im.cpuDeadline = im.cpuStart + options_.cpuLimitMs;
    im.wallDeadline = wall0 + std::max(4 * options_.cpuLimitMs, 2000.0);  // S1.4.2 backstop

    delegate_.taskStarted();
    std::string probeResult;
    switch (t.kind) {
    case Kind::Boot: im.call(protocol::kBoot, bootArg_); break;
    case Kind::Script: t.arg = im.run_script() ? "ran" : ""; break;
    case Kind::Loaded: im.call(protocol::kLoaded); break;
    case Kind::Timer:
        pendingFires_.erase(t.timerId);
        im.call(protocol::kTimer, t.arg);
        break;
    case Kind::Frame:
        frameRequested_ = false;
        if (!visible_) {
            frameWanted_ = true;  // S1.8: a frame granted while hidden is dropped, the request remembered
            break;
        }
        im.call(protocol::kFrame, t.arg);
        break;
    case Kind::Event: im.call(protocol::kEvent, t.arg); break;
    case Kind::Viewport: im.call(protocol::kViewport, t.arg); break;
    case Kind::Resize: im.call(protocol::kResize, t.arg); break;
    case Kind::Theme: im.call(protocol::kTheme, t.arg); break;
    case Kind::SafeArea: im.call(protocol::kSafeArea, t.arg); break;
    case Kind::Bridge: im.call(protocol::kBridge, t.arg); break;
    case Kind::Visibility: {
        visible_ = t.arg == "1";
        im.call(protocol::kVisibility, Json::object().set("visible", visible_).dump());
        if (visible_ && frameWanted_) {
            frameWanted_ = false;
            im.want_frame();
        }
        break;
    }
    case Kind::Probe: probeResult = im.probe(t.arg); break;
    }
    if (!dead_.load() && (t.kind != Kind::Script || !t.arg.empty())) im.settle();

    const double cpu = std::max(0.0, thread_cpu_ms() - im.cpuStart);
    im.cpuDeadline = 0;
    currentGesture_ = 0;
    inEvent_ = false;
    jsMs_ = jsMs_.load() + cpu;
    tasks_++;
    throttle_.chargeJs(now(), cpu);
    im.update_memory();
    // A limit that was crossed after the last interrupt poll still kills (S1.7 "also when the task ended…").
    if (!dead_.load() && killRequested_.load()) {
        std::string reason;
        {
            std::lock_guard l(mutex_);
            reason = killReason_;
        }
        if (!reason.empty()) die(reason);
    }
    if (!dead_.load() && !im.interruptReason.empty()) die(im.interruptReason);
    if (t.probe) t.probe->set_value(dead_.load() || stopping_ ? "Error: dead" : probeResult);
    // A script check that found no script was no task.
    if (t.kind == Kind::Script && t.arg != "ran" && !im.uncaught) {
        tasks_--;
        return;
    }
    TaskReport report{im.taskKind, cpu, steady_ms() - wall0, im.uncaught};
    if (!stopping_) delegate_.taskDone(report);
}

void Engine::loop() {
    // S1.2.3-4: boot, the inline scripts one by one, `loaded`; inputs wait until then (S1.2.5).
    {
        Task boot{Kind::Boot};
        runTask(boot);
        runQueuedScripts();
        if (dead_.load() || killRequested_.load()) goto dead;
        Task loaded{Kind::Loaded};
        runTask(loaded);
        if (dead_.load()) goto dead;
        runQueuedScripts();
        booted_ = true;
        delegate_.loaded();
    }
    for (;;) {
        Task task;
        {
            std::unique_lock l(mutex_);
            if (!waitForWork(l)) break;
            // waitForWork leaves the chosen task at the front.
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        runTask(task);
        if (dead_.load()) break;
        runQueuedScripts();
        if (dead_.load()) break;
    }
dead:
    if (!dead_.load() && killRequested_.load()) {
        std::string reason;
        {
            std::lock_guard l(mutex_);
            reason = killReason_;
        }
        if (!reason.empty() && !stopping_) die(reason);
    }
}

bool Engine::waitForWork(std::unique_lock<std::mutex>& l) {
    // Under mutex_. Picks the next task and moves it to the front; false when stopping or dead.
    for (;;) {
        if (stopping_ || dead_.load()) return false;
        if (killRequested_.load()) {
            std::string reason = killReason_;
            l.unlock();
            die(reason.empty() ? "stopped" : reason);
            l.lock();
            return false;
        }
        const double t = now();
        fireDueTimers(t);
        std::optional<double> wakeAt;  // card time
        if (!queue_.empty()) {
            if (neverDeferred(queue_.front().kind)) return true;
            Throttle::Decision d = throttle_.decide(t);
            if (d.run) return true;
            // Deferred (S1.7): one wake-up; events still pass.
            if (!queue_.front().deferredCounted) {
                queue_.front().deferredCounted = true;
                deferred_++;
            }
            throttle_.noteDeferred(t);
            if (t - lastThrottledDiag_ >= 1000) {
                lastThrottledDiag_ = t;
                delegate_.diagnostic("cpu-throttled", kindName(queue_.front().kind));
            }
            if (throttle_.throttledTooLong(t)) {
                l.unlock();
                die("cpu: throttled too long");
                l.lock();
                return false;
            }
            for (auto it = queue_.begin(); it != queue_.end(); ++it)
                if (neverDeferred(it->kind)) {
                    Task chosen = std::move(*it);
                    queue_.erase(it);
                    queue_.push_front(std::move(chosen));
                    return true;
                }
            wakeAt = d.until;
        }
        if (!timers_.empty()) {
            double next = 1e300;
            for (auto& [id, timer] : timers_) next = std::min(next, timer.due);
            if (!visible_) next = std::max(next, lastHiddenFire_ + kHiddenTimerMs);
            wakeAt = wakeAt ? std::min(*wakeAt, next) : next;
        }
        const size_t before = queue_.size();
        auto ready = [&] { return stopping_ || killRequested_.load() || queue_.size() != before; };
        if (wakeAt) {
            auto until = std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double, std::milli>(*wakeAt + bootSteadyMs_)));
            cv_.wait_until(l, until, ready);
        } else {
            cv_.wait(l, ready);
        }
    }
}

} // namespace twcard
