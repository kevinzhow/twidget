#pragma once

#include "twcard/engine.h"
#include "twcard/intl.h"
#include "twidget/gesture_policy.h"
#include "twidget/json.h"

#include <functional>
#include <chrono>
#include <map>

namespace twcard {

using StorageArea = std::map<std::string, std::string>;

struct Theme {
    std::string colorScheme = "light";
    twidget::Json params = twidget::Json::object();
    twidget::Json button = twidget::Json::object();
    twidget::Json text = twidget::Json::object();
};

struct Data {
    std::optional<std::string> chatData;
    std::optional<std::string> publicData;
};

struct Options {
    EngineOptions engine;
    double devicePixelRatio = 1;
    std::string policy = "strict";
    std::vector<std::string> features;
    std::string locale = "en-US";
    twidget::Json contentSafeArea = twidget::Json::object();
    struct { StorageArea local, session; } storage;
    Data data;
    struct {
        bool isCopy = false, canQuery = false, backgroundQuery = false, preview = false, wallet = false;
        std::optional<std::string> chatType;
        twidget::Json viewer;
    } context;
    std::shared_ptr<const IntlProvider> intl;
};

struct Action {
    enum class Kind { Ready, OpenLink, OpenTelegramLink, Haptic };
    Kind kind = Kind::Ready;
    std::string url, haptic, value;
    twidget::Json toJson() const;
};

struct Request {
    std::string data;
    bool wallet = false;
};

enum class RequestFailure { None, Timeout, RateLimited, Failed };
struct RequestOutcome {
    RequestFailure failure = RequestFailure::None;
    std::optional<std::string> result;
};

struct Diagnostics {
    bool loaded = false, rendered = false;
    std::optional<std::string> dead, lastError;
    std::optional<double> loadedMs, firstContentMs, renderedMs;
    double height = 50;
    std::string resizeAnchor = "top";
    bool watchesPublicData = false;
    size_t nodes = 0;
    struct {
        double jsMs = 0, validatorMs = 0;
        size_t memoryBytes = 0;
        int64_t ops = 0, throttledTasks = 0, tasks = 0;
    } stats;
    struct Diagnostic { size_t count = 0; std::vector<std::string> details; };
    std::map<std::string, Diagnostic> counters;
    std::vector<Action> actions;
};

/// The renderer's trusted seam (SPEC S10). Only post() may be called off the owner thread.
class Renderer {
public:
    virtual ~Renderer() = default;
    virtual void load() = 0;
    virtual void send(const std::string& message) = 0;
    virtual void post(std::function<void()> task) = 0;
    virtual void postAt(std::chrono::steady_clock::time_point due, std::function<void()> task) = 0;
    virtual void destroy() = 0;

    std::function<void(const std::string&)> onMessage;
    std::function<void(std::string_view, std::string_view)> onDiag;
};

/// Owns one renderer and one Engine (SPEC S10). Install observers before start().
/// Observers run on the renderer's owner thread. destroy() runs on that same thread.
class CardHost {
public:
    using RendererFactory = std::function<std::unique_ptr<Renderer>(const Theme&, double width)>;
    using Respond = std::function<void(const RequestOutcome&)>;

    CardHost(std::string html, Theme theme, double width, Options options,
             std::shared_ptr<const Resources> resources, RendererFactory factory);
    ~CardHost();
    CardHost(const CardHost&) = delete;
    CardHost& operator=(const CardHost&) = delete;

    void start();
    void setVisible(bool visible);
    void setData(Data data);
    void setTheme(Theme theme);
    void setContentSafeArea(twidget::Json inset);
    std::string probe(const std::string& code);
    void destroy();

    std::function<void(const Action&)> onAction;
    std::function<void(const Request&, Respond)> onRequest;
    std::function<void(const std::string&)> onCopyText;
    std::function<void(const std::string&, const StorageArea&)> onStorageCommit;
    std::function<void(const std::string&)> onError;
    std::function<void(const Diagnostics&)> onChange;
    std::function<void(twidget::gesture::Claim)> onGesture;
    std::function<void(bool)> onWatchPublicData;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace twcard
