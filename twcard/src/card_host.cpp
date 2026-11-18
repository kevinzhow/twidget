#include "twcard/card_host.h"

#include "css_sanitizer.h"
#include "profile.h"
#include "twcard/validator.h"
#include "twidget/paint_limits.h"
#include "twidget/theme.h"
#include "urls.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <stdexcept>

namespace twcard {
namespace {

using twidget::Json;
using Clock = std::chrono::steady_clock;

double cpuMs() {
    timespec time{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time);
    return double(time.tv_sec) * 1000 + double(time.tv_nsec) / 1e6;
}

double elapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

Json message(std::string_view type) { return Json::object().set("type", type); }

Json storageJson(const StorageArea& values) {
    Json result = Json::object();
    for (const auto& [name, value] : values) result.set(name, value);
    return result;
}

std::optional<StorageArea> checkedStorage(const Json& value) {
    // S7.4: UTF-8 bytes of keys and values, per area.
    constexpr size_t capacity = 4096;
    if (!value.isObject()) return std::nullopt;
    size_t size = 0;
    StorageArea result;
    for (const auto& [key, text] : value.obj()) {
        if (!text.isString()) return std::nullopt;
        size += key.size() + text.size();
        if (size > capacity) return std::nullopt;
        result.emplace(key, text.asString());
    }
    return result;
}

bool textWithin(std::string_view value, size_t cap) {
    return !value.empty() && value.size() <= cap && value.find('\0') == std::string_view::npos;
}

Json toJson(const std::optional<std::string>& value) { return value ? Json(*value) : Json(); }

bool isThemeDark(const Theme& theme) { return theme.colorScheme == "dark"; }

Theme checkedTheme(Theme theme) {
    const bool dark = isThemeDark(theme);
    theme.colorScheme = dark ? "dark" : "light";
    theme.params = twidget::theme_params(theme.params, dark);
    if (!theme.button.isObject()) theme.button = Json::object();
    if (!theme.text.isObject()) theme.text = Json::object();
    return theme;
}

Json viewportJson(double width, double height, double dpr) {
    return Json::object().set("innerWidth", width).set("innerHeight", height).set("dpr", dpr);
}

} // namespace

Json Action::toJson() const {
    switch (kind) {
    case Kind::Ready: return Json::object().set("kind", "ready");
    case Kind::OpenLink: return Json::object().set("kind", "openLink").set("url", url);
    case Kind::OpenTelegramLink: return Json::object().set("kind", "openTelegramLink").set("url", url);
    case Kind::Haptic: return Json::object().set("kind", "haptic").set("haptic", haptic).set("value", value);
    }
    return Json::object();
}

struct CardHost::Impl : EngineDelegate, std::enable_shared_from_this<Impl> {
    Impl(CardHost& host, std::string html, Theme theme, double width, Options options,
         std::shared_ptr<const Resources> resources, const RendererFactory& factory)
        : host(host), html(std::move(html)), theme(checkedTheme(std::move(theme))), options(std::move(options)),
          renderer(factory(this->theme, width)), viewport(viewportJson(400, 300, 1)),
          validator({this->options.policy == "compat"},
                    [this](std::string_view kind, std::string_view detail) { diagnostic(kind, detail); }),
          engine(std::move(resources), this->options.engine, *this) {
        if (!renderer) throw std::invalid_argument("missing card renderer");
        if (this->options.policy != "compat") this->options.policy = "strict";
        this->options.contentSafeArea = twidget::content_safe_area(this->options.contentSafeArea);
        checkData();
    }

    CardHost& host;  // Used only on the owner thread while the host is open.
    std::string html;
    Theme theme;
    Options options;
    std::unique_ptr<Renderer> renderer;
    Json viewport;
    Diagnostics diagnostics;
    std::atomic<bool> closed{false};
    bool started = false, ready = false, sentOps = false;
    int64_t nextGesture = 0, gesture = 0;
    bool pressOpen = false;
    Clock::time_point startedAt;

    std::mutex queryMutex;
    std::condition_variable queryReady;
    bool queryAborted = false;
    int64_t nextQuery = 0, activeQuery = 0;
    std::optional<Json> queryResult;
    std::optional<std::vector<uint8_t>> queryBinary;

    // Only read or written on the engine's thread.
    std::optional<std::vector<uint8_t>> lastBinary;
    size_t nodes = 0;
    int64_t ops = 0;
    double validatorMs = 0;
    Validator validator;
    Engine engine;

    void post(std::function<void(Impl&)> work) {
        if (closed) return;
        renderer->post([weak = weak_from_this(), work = std::move(work)] {
            if (auto self = weak.lock(); self && !self->closed) work(*self);
        });
    }

    void bind() {
        renderer->onMessage = [weak = weak_from_this()](const std::string& wire) {
            if (auto self = weak.lock()) {
                auto value = Json::parse(wire);
                if (value && value->isObject())
                    self->post([value = std::move(*value)](Impl& h) { h.onMessage(value); });
                else
                    self->diagnostic("renderer-violation", "invalid message");
            }
        };
        renderer->onDiag = [weak = weak_from_this()](std::string_view kind, std::string_view detail) {
            if (auto self = weak.lock()) self->diagnostic(kind, detail);
        };
    }

    void send(Json value) { renderer->send(value.dump()); }

    void sendTheme() {
        send(message("theme").set("theme", Json::object().set("colorScheme", theme.colorScheme)
             .set("params", theme.params).set("button", theme.button).set("text", theme.text)));
    }

    void start() {
        if (started) return;
        started = true;
        startedAt = Clock::now();
        sendTheme();
        send(message("safeArea").set("inset", options.contentSafeArea));
        send(message("policy").set("policy", options.policy));
        renderer->load();
    }

    void checkData() {
        // SPEC S7.5: absent and invalid data both become null.
        for (auto* value : {&options.data.chatData, &options.data.publicData}) {
            if (*value && !textWithin(**value, 1024)) {
                value->reset();
                addDiag("data-rejected", "invalid card data");
            }
        }
        if (options.context.isCopy) options.data.chatData.reset();
    }

    Json dataMessage() const {
        return message("data").set("chatData", toJson(options.data.chatData))
            .set("publicData", toJson(options.data.publicData));
    }

    Json bootArg() {
        auto area = [this](const StorageArea& values) {
            if (options.context.preview) return Json::object();
            auto packed = storageJson(values);
            if (checkedStorage(packed)) return packed;
            addDiag("storage-rejected", "preload exceeds quota");
            return Json::object();
        };
        const bool hideContext = options.context.isCopy || options.context.preview;
        Json features = Json::array();
        for (const auto& feature : options.features) features.push(feature);
        const auto& locale = options.locale;
        const bool localeShape = !locale.empty() && locale.size() <= 35 &&
            locale.front() != '-' && locale.back() != '-' && locale.find("--") == std::string::npos &&
            locale.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-") == std::string::npos;

        Json config = Json::object().set("maxTimers", options.engine.maxTimers).set("policy", options.policy)
            .set("visible", options.engine.visible).set("viewport", viewport)
            .set("theme", theme.colorScheme).set("themeParams", theme.params)
            .set("contentSafeArea", options.contentSafeArea).set("locale", localeShape ? locale : "en-US")
            .set("storage", Json::object().set("local", area(options.storage.local))
                                         .set("session", area(options.storage.session)))
            .set("chatData", toJson(options.data.chatData)).set("publicData", toJson(options.data.publicData))
            .set("isCopy", options.context.isCopy).set("canQuery", options.context.canQuery)
            .set("isPreview", options.context.preview).set("profile", profile::kProfile)
            .set("chatType", hideContext ? Json() : toJson(options.context.chatType))
            .set("viewer", Json()).set("wallet", options.context.wallet).set("features", std::move(features));
        return Json::object().set("html", std::move(html)).set("config", std::move(config));
    }

    void notify() {
        if (!closed && host.onChange) host.onChange(diagnostics);
    }

    void addDiag(std::string_view kind, std::string_view detail) {
        // S10.5 retains the first few details of each diagnostic kind.
        if (kind.empty() || kind.size() > 64) return;
        if (diagnostics.counters.size() >= 128 && !diagnostics.counters.contains(std::string(kind))) return;
        auto& entry = diagnostics.counters[std::string(kind)];
        ++entry.count;
        if (entry.details.size() < 4) entry.details.emplace_back(detail.substr(0, 512));
    }

    void onMessage(const Json& value) {
        const auto& kind = value["type"].asString();
        if (kind == "ready") {
            if (!ready && started) {
                ready = true;
                try {
                    engine.start(bootArg().dump());
                } catch (const std::exception& error) {
                    dead(error.what());
                }
            }
        } else if (kind == "result") {
            response(value);
        } else if (kind == "size") {
            const double height = value["height"].asNumber(diagnostics.height);
            diagnostics.height = std::clamp(height, 50.0, 600.0);  // S10.3
            if (sentOps && !diagnostics.firstContentMs) diagnostics.firstContentMs = elapsedMs(startedAt);
            auto next = viewportJson(value["innerWidth"].asNumber(viewport["innerWidth"].asNumber()),
                                     value["innerHeight"].asNumber(viewport["innerHeight"].asNumber()),
                                     value["dpr"].asNumber(viewport["dpr"].asNumber()));
            if (next != viewport) {
                viewport = std::move(next);
                if (ready) engine.post(Engine::Input::Viewport, viewport.dump());
            }
            notify();
        } else if (kind == "event" && !options.context.preview && ready) {
            const auto& event = value["ev"];
            const auto& type = event["type"].asString();
            if (type == "pointerdown") {
                pressOpen = true;
                gesture = 0;
            }
            const bool activate = value["trusted"].asBool() &&
                (type == "pointerdown" || type == "mousedown" || type == "pointerup" || type == "mouseup" ||
                 type == "click" || type == "dblclick");
            if (activate && !gesture && (pressOpen || type != "click" || event["detail"].asNumber() > 0))
                gesture = ++nextGesture;
            engine.post(Engine::Input::Event, value.dump(), activate ? gesture : 0);
            if (type == "pointercancel" || type == "click" || type == "dblclick") pressOpen = false;
        } else if (kind == "resize" && ready) {
            engine.post(Engine::Input::Resize, Json::object().set("entries", value["entries"]).dump());
        } else if (kind == "frame" && ready && value["time"].isNumber()) {
            engine.frame(value["time"].asNumber());
        } else if (kind == "gesture") {
            gesture = 0;
            pressOpen = true;
            if (host.onGesture) host.onGesture({value["x"].asBool(), false, value["hold"].asBool()});
        } else if (kind == "release") {
            // S3.4: the renderer's following pointerup and click still belong to this press.
        } else if (kind == "limit") {
            const auto limit = twidget::paint_limit_from_code(value["code"].asString());
            const auto count = value["count"].asNumber();
            if (!limit || !value["count"].isNumber() || std::trunc(count) != count || count > INT32_MAX ||
                count <= (*limit == twidget::PaintLimit::Paint ? twidget::kMaxHeavyBoxes : twidget::kMaxBackdropBoxes)) {
                addDiag("renderer-violation", "invalid paint limit");
            } else if (options.policy != "compat") {
                engine.kill(twidget::paint_limit_reason(*limit, int(count)));
            }
        } else if (kind == "violation") {
            addDiag("renderer-violation", value["detail"].asString());
        }
    }

    void response(const Json& value) {
        const int64_t qid = value["qid"].asInt64();
        if (qid == -1) {
            if (!diagnostics.dead && !diagnostics.rendered) {
                diagnostics.rendered = true;
                diagnostics.renderedMs = elapsedMs(startedAt);
                notify();
            }
            return;
        }
        std::lock_guard lock(queryMutex);
        if (!qid || qid != activeQuery || queryAborted || queryResult) return;
        queryResult = value["result"];
        if (value["binaryBase64"].isString())
            queryBinary = urls::forgiving_base64_decode(value["binaryBase64"].asString());
        queryReady.notify_one();
    }

    HostValue ask(Json query, double& mainThreadMs) {
        const auto start = Clock::now();
        std::unique_lock lock(queryMutex);
        const auto deadAnswer = [] { return HostValue::string(R"({"error":"dead"})"); };
        if (queryAborted) return deadAnswer();
        const int64_t id = activeQuery = ++nextQuery;
        queryResult.reset();
        queryBinary.reset();
        lock.unlock();
        post([id, query = std::move(query)](Impl& h) mutable {
            if (query["q"].asString() == "matchMedia") {
                const auto checked = CssSanitizer().mediaQueryList(query["query"].asString(), isThemeDark(h.theme));
                query.set("query", checked.value_or("not all"));
            }
            h.send(message("query").set("qid", id).set("query", query));
        });
        lock.lock();

        // S1.3: a renderer answer, a stop, or the blocking query's three-second deadline.
        const bool arrived = queryReady.wait_until(lock, start + std::chrono::seconds(3), [this] {
            return queryAborted || queryResult.has_value();
        });
        activeQuery = 0;
        mainThreadMs = elapsedMs(start);
        if (queryAborted) return deadAnswer();
        if (!arrived) return HostValue::string(R"({"error":"timeout"})");
        lastBinary = std::move(queryBinary);
        return HostValue::string(queryResult->dump());
    }

    void storage(std::string_view arg) {
        // S7.4: refuse large messages without parsing.
        if (arg.size() > 100000 || options.context.preview) return diagnostic("storage-rejected", "invalid commit");
        const auto input = Json::parse(arg);
        if (!input || !input->isObject()) return diagnostic("storage-rejected", "invalid commit");
        for (const auto& [area, contents] : input->obj()) {
            auto values = checkedStorage(contents);
            if ((area != "local" && area != "session") || !values) {
                diagnostic("storage-rejected", area);
                continue;
            }
            post([area, values = std::move(*values)](Impl& h) {
                if (h.host.onStorageCommit) h.host.onStorageCommit(area, values);
            });
        }
    }

    void action(Action value) {
        if (diagnostics.actions.size() < 1000) diagnostics.actions.push_back(value);  // S10.5
        if (host.onAction) host.onAction(value);
    }

    HostValue hostCall(std::string_view kind, std::string_view arg, double& mainThreadMs) override {
        using Result = HostValue;
        if (kind == "ops") {
            const double start = cpuMs();
            auto accepted = validator.ops(arg);
            validatorMs += cpuMs() - start;
            if (accepted.stop) {
                engine.kill(*accepted.stop);
                return {};
            }
            nodes = validator.documentNodes();
            if (!accepted.ops.empty()) {
                ops += int64_t(accepted.ops.size());
                mainThreadMs = Engine::opsCostMs(double(accepted.ops.size()), double(accepted.canvasCommands));
                post([value = std::move(accepted.ops)](Impl& h) {
                    h.sentOps = true;
                    h.send(message("ops").set("ops", value));
                });
            }
        } else if (kind == "query") {
            lastBinary.reset();
            const double start = cpuMs();
            std::string error;
            auto query = validator.query(arg, error);
            validatorMs += cpuMs() - start;
            if (!query) return Result::string(Json::object().set("error", error).dump());
            return ask(std::move(*query), mainThreadMs);
        } else if (kind == "listen" || kind == "pd" || kind == "anim") {
            const double start = cpuMs();
            auto result = kind == "listen" ? validator.listen(arg) :
                          kind == "pd" ? validator.preventDefault(arg) : validator.anim(arg);
            validatorMs += cpuMs() - start;
            if (!result) return {};
            Json out;
            if (kind == "listen") out = message("listen").set("types", std::move(*result));
            else if (kind == "pd") out = message("preventDefault").set("keys", std::move(*result));
            else out = message("anim").set("now", engine.now()).set("anims", std::move(*result));
            post([out = std::move(out)](Impl& h) { h.send(out); });
        } else if (kind == "log" || kind == "diag") {
            const auto delimiter = arg.find(':');
            diagnostic(kind == "log" ? kind : arg.substr(0, delimiter),
                       delimiter == std::string_view::npos || kind == "log" ? arg : arg.substr(delimiter + 1));
        } else if (kind == "storage") {
            storage(arg);
        } else if (kind == "bridge") {
            const auto value = Json::parse(arg);
            if (value && value->isArray() && value->size() == 2 && (*value)[0].asString() == "ready")
                post([](Impl& h) { h.action({}); });
        } else if (kind == "intl" && options.intl) {
            if (auto value = options.intl(arg)) return Result::string(std::move(*value));
        } else if (kind == "url") {
            const auto request = Json::parse(arg);
            if (!request || !(*request)["input"].isString()) return Result::null();
            const auto* base = request->find("base");
            if (base && !base->isString()) return Result::null();
            auto url = urls::parse((*request)["input"].asString(),
                                   base ? std::optional<std::string_view>(base->asString()) : std::nullopt);
            return url ? Result::string(url->dump()) : Result::null();
        }
        return {};
    }

    std::optional<std::vector<uint8_t>> hostBinary() override { return lastBinary; }

    void requestFrame() override {
        post([](Impl& h) { h.send(message("requestFrame")); });
    }

    void loaded() override {
        post([](Impl& h) {
            h.diagnostics.loaded = true;
            h.diagnostics.loadedMs = elapsedMs(h.startedAt);
            h.notify();
            if (!h.closed) h.send(message("query").set("qid", -1).set("query", Json::object().set("q", "viewport")));
        });
    }

    void dead(const std::string& reason) override {
        post([reason](Impl& h) {
            if (h.diagnostics.dead) return;
            h.diagnostics.dead = reason;
            h.send(message("frozen").set("reason", "Widget stopped").set("fatal", true));
            h.notify();
        });
    }

    void diagnostic(std::string_view kind, std::string_view detail) override {
        post([kind = std::string(kind.substr(0, 64)), detail = std::string(detail.substr(0, 512))](Impl& h) {
            h.addDiag(kind, detail);
        });
    }

    void uncaughtError(const std::string& text) override {
        post([text](Impl& h) {
            h.diagnostics.lastError = text;
            h.addDiag("error", text);
            if (h.host.onError) h.host.onError(text);
        });
    }

    void taskStarted() override { validator.beginTask(); }

    void taskDone(const TaskReport& report) override {
        Diagnostics result;
        result.nodes = nodes;
        result.stats = {engine.jsMs(), validatorMs, engine.memoryBytes(), ops, engine.deferredTasks(), engine.tasks()};
        post([stats = result.stats, nodes = result.nodes, clean = !report.uncaught](Impl& h) {
            h.diagnostics.stats = stats;
            h.diagnostics.nodes = nodes;
            if (clean) h.diagnostics.lastError.reset();
            h.notify();
        });
    }

    void abortBlocking() override {
        std::lock_guard lock(queryMutex);
        queryAborted = true;
        queryReady.notify_all();
    }

    void destroy() {
        if (closed.exchange(true)) return;
        renderer->onMessage = {};
        renderer->onDiag = {};
        engine.stop();
        renderer->destroy();
    }
};

CardHost::CardHost(std::string html, Theme theme, double width, Options options,
                   std::shared_ptr<const Resources> resources, RendererFactory factory)
    : impl_(std::make_shared<Impl>(*this, std::move(html), std::move(theme), width, std::move(options),
                                   std::move(resources), factory)) {
    impl_->bind();
}

CardHost::~CardHost() { destroy(); }

void CardHost::start() { impl_->post([](Impl& h) { h.start(); }); }

void CardHost::setVisible(bool visible) {
    impl_->post([visible](Impl& h) {
        h.options.engine.visible = visible;
        h.engine.setVisible(visible);
    });
}

void CardHost::setData(Data data) {
    impl_->post([data = std::move(data)](Impl& h) {
        h.options.data = data;
        h.checkData();
        if (h.ready) h.engine.post(Engine::Input::Bridge, h.dataMessage().dump());
    });
}

void CardHost::setTheme(Theme theme) {
    impl_->post([theme = std::move(theme)](Impl& h) {
        h.theme = checkedTheme(theme);
        if (h.started) h.sendTheme();
        if (h.ready)
            h.engine.post(Engine::Input::Theme, Json::object().set("colorScheme", h.theme.colorScheme)
                          .set("themeParams", h.theme.params).dump());
    });
}

void CardHost::setContentSafeArea(Json inset) {
    impl_->post([inset = std::move(inset)](Impl& h) {
        h.options.contentSafeArea = twidget::content_safe_area(inset);
        auto arg = Json::object().set("inset", h.options.contentSafeArea);
        if (h.started) h.send(message("safeArea").set("inset", h.options.contentSafeArea));
        if (h.ready) h.engine.post(Engine::Input::SafeArea, arg.dump());
    });
}

std::string CardHost::probe(const std::string& code) { return impl_->engine.probe(code); }

void CardHost::destroy() { impl_->destroy(); }

} // namespace twcard
