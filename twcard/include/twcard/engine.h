// twcard::Engine (SPEC S1): one QuickJS runtime and context per card, on a thread of its own, running the card's
// tasks under the card's limits. The engine knows nothing of the DOM, the renderer or the app: the runtime bundle
// (card world, S2–S8) does the card's work, and everything the bundle asks of the outside goes to an EngineDelegate
// (CardHost, S10), except the few host functions the engine answers itself (now, timer, clearTimer, raf, random).
//
// Threads: every EngineDelegate call happens on the engine's thread. Engine methods may be called from any thread;
// only probe() blocks (until its task ran) and stop() joins the thread.
#pragma once

#include "twcard/throttle.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace twcard {

/// The protocol number this engine speaks with the runtime bundle (SPEC "Version"). A bundle whose first line names
/// another one is refused.
inline constexpr int kProtocol = 1;

/// The runtime bundle (SPEC S0 "Version"): its source, whose first line is `// twcard <semver> protocol <n>`, and
/// optionally the same bundle precompiled (QuickJS bytecode, made at build time by our own tool, never from the card).
struct Resources {
    std::string bundle;
    /// `twcard-bytecode <the bundle's first line>\n` followed by JS_WriteObject's output; ignored when the line does
    /// not match the bundle's (diagnostic `bytecode`).
    std::vector<uint8_t> bytecode;
    /// `twcard-runtime.js` (+ `twcard-runtime.qbc` when present) in `dir`; throws std::runtime_error when the
    /// bundle cannot be read.
    static std::shared_ptr<Resources> fromDirectory(const std::string& dir);
    /// The first line's semver and protocol number; nullopt when the line is not of that form.
    static std::optional<std::pair<std::string, int>> header(std::string_view bundle);
};

struct EngineOptions {
    size_t memoryLimitBytes = size_t(8) << 20;  // S1.1
    double cpuLimitMs = 500;                    // S1.4
    Throttle::Limits throttle;                  // S1.7
    int maxTimers = 64;                         // S1.6
    bool visible = true;                        // S1.8
};

/// A host function's answer to the card world.
struct HostValue {
    enum class Kind { Undefined, Null, String, Number } kind = Kind::Undefined;
    std::string text;
    double number = 0;
    static HostValue undefined() { return {}; }
    static HostValue null() { return {Kind::Null, {}, 0}; }
    static HostValue string(std::string s) { return {Kind::String, std::move(s), 0}; }
    static HostValue num(double n) { return {Kind::Number, {}, n}; }
};

/// What one task did (stats, S10.5).
struct TaskReport {
    std::string kind;
    double jsMs = 0;         // thread CPU time of the task, validator included (S1.4.6)
    double wallMs = 0;
    bool uncaught = false;   // an error escaped card code in it (S1.4.7; lastError is cleared by a clean task)
};

/// Everything the engine asks of its host. Called on the engine's thread only.
class EngineDelegate {
public:
    virtual ~EngineDelegate() = default;
    /// host(kind, arg) for the kinds the engine does not answer itself: ops, query, log, diag, listen, pd, anim,
    /// bridge, storage, intl, stat, url (S1.3). `arg` has passed the engine's length cap. Set `mainThreadMs` to what
    /// the call cost the renderer (S1.7: query wall time, opsCostMs of a batch).
    virtual HostValue hostCall(std::string_view kind, std::string_view arg, double& mainThreadMs) = 0;
    /// The binary half of the last query answer (S1.3 hostBinary); nullopt: null.
    virtual std::optional<std::vector<uint8_t>> hostBinary() { return std::nullopt; }
    /// The card wants one animation frame (S4.2): ask the renderer (`requestFrame`).
    virtual void requestFrame() = 0;
    /// The inline scripts ran and `loaded` was dispatched (S1.2.4).
    virtual void loaded() = 0;
    /// The card died (S1.9), once. The engine runs no more card code after this.
    virtual void dead(const std::string& reason) = 0;
    /// A diagnostic of the engine's own (kind, detail).
    virtual void diagnostic(std::string_view kind, std::string_view detail) = 0;
    /// An error that escaped card code, already reported to the card's own `error` listeners where possible (S1.4.7).
    virtual void uncaughtError(const std::string& text) = 0;
    virtual void taskDone(const TaskReport&) {}
    /// From any thread: the engine is stopping or the card died; a synchronous wait inside hostCall (a query) must
    /// return now (S1.9: "answer a pending query with {"error":"dead"}").
    virtual void abortBlocking() {}
};

class Engine {
public:
    /// Host inputs that become tasks (S1.4).
    enum class Input { Event, Frame, Viewport, Resize, Theme, SafeArea, Bridge };

    Engine(std::shared_ptr<const Resources> resources, EngineOptions options, EngineDelegate& delegate);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// Starts the thread and boots (S1.2) with `bootArg` = the JSON `{html, config}` of dispatch `boot`.
    void start(std::string bootArg);
    /// A task input; `arg` is the dispatch argument (JSON). `gesture`: for Event, the host's gesture number of a
    /// trusted event (S3.4), 0 for none. Inputs before the boot finished wait for it (S1.2.5).
    void post(Input input, std::string arg, int64_t gesture = 0);
    /// A renderer frame message (S4.2) at `steadyMs` (std::chrono::steady_clock, ms).
    void frame(double steadyMs);
    /// S1.8.
    void setVisible(bool visible);
    /// S1.10: `code` evaluated as global code in the card's context, as a task; its result as String(value), an
    /// exception as `Error: message`. Blocks until done. "Error: dead" once the card died.
    std::string probe(const std::string& code);
    /// Stops the card with `reason` (a validator cap, the paint limit, …): S1.9. Any thread, idempotent.
    void kill(const std::string& reason);
    /// Stops the thread and frees the runtime; idempotent. No delegate call happens after it returns.
    void stop();

    bool isDead() const { return dead_.load(); }
    /// ms since boot on the card's clock (the `now` host function before coarsening).
    double now() const;
    /// A steady_clock time (ms) on the card's clock.
    double toCardTime(double steadyMs) const { return steadyMs - bootSteadyMs_; }

    /// On the engine's thread, during a task: the gesture number of the event it dispatches (0: none) — the user
    /// activation the host checks a privileged call against (S3.4). Promise jobs drained in the same task see it too.
    int64_t currentGesture() const { return currentGesture_; }
    /// On the engine's thread: whether the running task dispatches an event.
    bool inEventTask() const { return inEvent_; }
    /// On the engine's thread: whether the card is visible (as of the last visibility task).
    bool visibleNow() const { return visible_; }

    /// Stats (any thread).
    double jsMs() const { return jsMs_.load(); }
    int64_t tasks() const { return tasks_.load(); }
    int64_t deferredTasks() const { return deferred_.load(); }
    size_t memoryBytes() const { return memory_.load(); }

    static double opsCostMs(double ops, double commands) { return Throttle::opsCostMs(ops, commands); }

    struct Impl;  // the QuickJS side (engine.cpp)

private:
    enum class Kind { Boot, Script, Loaded, Timer, Frame, Event, Viewport, Resize, Theme, SafeArea, Bridge, Visibility, Probe };
    struct Task {
        Task() = default;
        explicit Task(Kind k, std::string a = {}) : kind(k), arg(std::move(a)) {}
        Kind kind = Kind::Boot;
        std::string arg;
        int64_t gesture = 0;
        int64_t timerId = 0;
        bool deferredCounted = false;  // counted once in deferredTasks
        std::shared_ptr<std::promise<std::string>> probe;
    };
    static const char* kindName(Kind k);
    static bool neverDeferred(Kind k) { return k == Kind::Event || k == Kind::Probe || k == Kind::Visibility; }

    void threadMain();
    void loop();
    bool waitForWork(std::unique_lock<std::mutex>& l);
    void enqueue(Task t);
    void fireDueTimers(double now);
    void runTask(Task& t);
    void runQueuedScripts();
    void die(const std::string& reason);

    friend struct Impl;
    std::shared_ptr<const Resources> resources_;
    EngineOptions options_;
    EngineDelegate& delegate_;
    std::unique_ptr<Impl> impl_;
    Throttle throttle_;
    double bootSteadyMs_ = 0;
    std::string bootArg_;

    // Shared with other threads (mutex_).
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> queue_;
    bool started_ = false, stopping_ = false, joined_ = false;
    std::atomic<bool> killRequested_{false}, dead_{false};
    std::string killReason_;  // mutex_

    // Engine thread only.
    bool booted_ = false;
    bool visible_ = true;
    int64_t currentGesture_ = 0;
    bool inEvent_ = false;
    struct Timer {
        double due;
    };
    std::map<int64_t, Timer> timers_;
    std::set<int64_t> pendingFires_;
    double lastHiddenFire_ = -1e18;
    bool frameRequested_ = false, frameWanted_ = false;
    double lastThrottledDiag_ = -1e18;

    std::atomic<double> jsMs_{0};
    std::atomic<int64_t> tasks_{0}, deferred_{0};
    std::atomic<size_t> memory_{0};
};

} // namespace twcard
