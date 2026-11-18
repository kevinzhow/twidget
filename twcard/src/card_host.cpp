#include "twcard/card_host.h"

#include "css_sanitizer.h"
#include "profile.h"
#include "twcard/validator.h"
#include "twidget/paint_limits.h"
#include "twidget/theme.h"
#include "urls.h"
#include "validation.h"

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

size_t utf16Units(std::string_view text) {
    size_t units = 0;
    for (unsigned char byte : text) {
        if ((byte & 0xc0) != 0x80) units += byte >= 0xf0 ? 2 : 1;
    }
    return units;
}

bool wellFormed(std::string_view text) {
    for (size_t i = 0; i + 2 < text.size(); ++i)
        if (static_cast<unsigned char>(text[i]) == 0xed &&
            (static_cast<unsigned char>(text[i + 1]) & 0xe0) == 0xa0) return false;
    return true;
}

bool oneOf(std::string_view value, std::initializer_list<std::string_view> allowed) {
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
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
        checkContext();
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
    int64_t consumedGesture = 0;
    bool pressOpen = false;
    Clock::time_point startedAt;
    std::optional<Clock::time_point> pressedAt, releasedAt, hapticSeries, hapticRefilledAt, hapticDiagAt;
    double hapticTokens = 5;  // S7.3
    struct IncomingRequest { int64_t id; Request value; };
    struct PendingRequest { int64_t id; uint64_t generation; Clock::time_point deadline; };
    std::optional<IncomingRequest> heldRequest;
    std::optional<PendingRequest> pendingRequest;
    std::optional<Clock::time_point> lastBackgroundRequest;
    uint64_t requestGeneration = 0;

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

    void postAt(Clock::time_point due, std::function<void(Impl&)> work) {
        renderer->postAt(due, [weak = weak_from_this(), work = std::move(work)] {
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

    void checkContext() {
        // SPEC S7.5: context fields are independent and viewer data is opt-in.
        auto& context = options.context;
        if (context.isCopy || context.preview) context.chatType.reset();
        if (context.chatType && *context.chatType != "private" && *context.chatType != "group" &&
            *context.chatType != "supergroup" && *context.chatType != "channel") {
            context.chatType.reset();
            addDiag("data-rejected", "chatType");
        }
        Json original = std::move(context.viewer);
        context.viewer = Json();
        if (!context.canQuery || context.preview || original.isNull()) return;
        const auto& id = original["id"];
        const double value = id.asNumber();
        if (!id.isNumber() || value < 1 || value > 9007199254740991.0 || std::trunc(value) != value) {
            addDiag("data-rejected", "viewer.id");
            return;
        }
        context.viewer = Json::object().set("id", id);
        for (std::string_view key : {"first_name", "last_name"}) {
            if (const auto* name = original.find(key)) {
                if (name->isString() && !name->empty() && utf16Units(name->asString()) <= 64)
                    context.viewer.set(key, *name);
                else
                    addDiag("data-rejected", key);
            }
        }
        if (const auto* username = original.find("username")) {
            const auto& name = username->asString();
            if (name.size() >= 5 && name.size() <= 32 &&
                name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos)
                context.viewer.set("username", name);
            else
                addDiag("data-rejected", "viewer.username");
        }
        if (original["is_premium"].asBool()) context.viewer.set("is_premium", true);
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
            .set("chatType", toJson(options.context.chatType))
            .set("viewer", options.context.viewer).set("wallet", options.context.wallet).set("features", std::move(features));
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
            const bool click = type == "click" || type == "dblclick";
            const bool activate = value["trusted"].asBool() &&
                (!click || event["detail"].asNumber() > 0 || pressOpen) &&
                (type == "pointerdown" || type == "mousedown" || type == "pointerup" || type == "mouseup" ||
                 click);
            if (activate && !gesture) gesture = ++nextGesture;
            engine.post(Engine::Input::Event, value.dump(), activate ? gesture : 0);
            if (type == "pointercancel" || click) pressOpen = false;
        } else if (kind == "resize" && ready) {
            engine.post(Engine::Input::Resize, Json::object().set("entries", value["entries"]).dump());
        } else if (kind == "frame" && ready && value["time"].isNumber()) {
            engine.frame(value["time"].asNumber());
        } else if (kind == "gesture") {
            gesture = 0;
            pressOpen = true;
            pressedAt = Clock::now();
            releasedAt.reset();
            hapticSeries.reset();
            if (host.onGesture) host.onGesture({value["x"].asBool(), false, value["hold"].asBool()});
        } else if (kind == "release") {
            // S3.4: the renderer's following pointerup and click still belong to this press.
            if (pressedAt && !releasedAt) releasedAt = Clock::now();
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

    void policyDiag(std::string_view kind, std::string_view detail) {
        // SPEC S7.3 caps reported refusals per diagnostic kind.
        const auto it = diagnostics.counters.find(std::string(kind));
        if (it == diagnostics.counters.end() || it->second.count < 20) addDiag(kind, detail);
    }

    bool activation(int64_t number, bool visible) const {
        return visible && options.engine.visible && number > consumedGesture;
    }

    bool useActivation(int64_t number, bool visible) {
        if (!activation(number, visible)) return false;
        consumedGesture = number;
        return true;
    }

    bool allowHaptic(Clock::time_point now, bool visible) {
        using namespace std::chrono_literals;
        if (!visible || !options.engine.visible) return false;
        if (hapticRefilledAt) {
            // SPEC S7.3: 10 tokens per second, up to 5.
            const double elapsed = std::chrono::duration<double>(now - *hapticRefilledAt).count();
            hapticTokens = std::min(5.0, hapticTokens + elapsed * 10);
        }
        hapticRefilledAt = now;
        const bool touch = pressedAt && now < std::min(releasedAt.value_or(*pressedAt + 30s), *pressedAt + 30s) + 5s;
        if (!touch && (!hapticSeries || now - *hapticSeries >= 1s)) {
            if (hapticSeries && now - *hapticSeries < 10s) return false;
            hapticSeries = now;
        }
        if (hapticTokens < 1) return false;
        --hapticTokens;
        return true;
    }

    void answer(int64_t id, const Json& result) {
        engine.post(Engine::Input::Bridge, message("answer").set("id", id).set("result", result).dump());
    }

    void reject(int64_t id, std::string_view error, std::string_view reason = {}) {
        auto response = message("answer").set("id", id).set("error", error);
        if (!reason.empty()) response.set("reason", reason);
        engine.post(Engine::Input::Bridge, response.dump());
    }

    void respond(uint64_t generation, const RequestOutcome& outcome, Clock::time_point receivedAt) {
        if (!pendingRequest || pendingRequest->generation != generation) return;
        const auto id = pendingRequest->id;
        const bool expired = receivedAt >= pendingRequest->deadline;
        pendingRequest.reset();
        if (expired) return reject(id, "TIMEOUT");
        switch (outcome.failure) {
        case RequestFailure::Timeout: reject(id, "TIMEOUT"); return;
        case RequestFailure::RateLimited: reject(id, "RATE_LIMITED"); return;
        case RequestFailure::Failed: reject(id, "FAILED"); return;
        case RequestFailure::None: break;
        }
        if (outcome.result && (outcome.result->size() > 1024 || outcome.result->find('\0') != std::string::npos ||
                               !wellFormed(*outcome.result))) {
            policyDiag("answer-rejected", "request result");
            reject(id, "INVALID_ANSWER");
        } else {
            answer(id, toJson(outcome.result));
        }
    }

    void submitRequest(IncomingRequest request, bool background) {
        const auto now = Clock::now();
        if (background) lastBackgroundRequest = now;
        const uint64_t generation = ++requestGeneration;
        // SPEC S7.3: the host answers after 15 seconds even if the app never calls respond.
        const auto deadline = now + std::chrono::seconds(15);
        pendingRequest = PendingRequest{request.id, generation, deadline};
        postAt(deadline, [generation, deadline](Impl& owner) {
            owner.respond(generation, {RequestFailure::Timeout, std::nullopt}, deadline);
        });
        host.onRequest(request.value, [weak = weak_from_this(), generation](const RequestOutcome& outcome) {
            const auto receivedAt = Clock::now();
            if (auto self = weak.lock()) self->post([generation, outcome, receivedAt](Impl& owner) {
                owner.respond(generation, outcome, receivedAt);
            });
        });
    }

    void request(IncomingRequest incoming, int64_t number, bool visible) {
        auto blocked = [&](std::string_view error, std::string_view reason = {}) {
            policyDiag("request-blocked", reason.empty() ? error : reason);
            reject(incoming.id, error, reason);
        };
        if (!options.context.canQuery) return blocked("NOT_ALLOWED", "disabled");
        if (pendingRequest) return blocked("BUSY");
        const bool foreground = activation(number, visible) || (options.policy == "compat" && visible && options.engine.visible);
        if (!foreground && !options.context.backgroundQuery) return blocked("NOT_ALLOWED", "no-activation");
        if (options.context.preview) return blocked("NOT_ALLOWED", "preview");
        if (!host.onRequest) return blocked("NOT_ALLOWED", "unsupported");
        if (foreground) {
            useActivation(number, visible);
            submitRequest(std::move(incoming), false);
        } else if (!visible || !options.engine.visible) {
            if (heldRequest) reject(heldRequest->id, "BUSY");
            heldRequest = std::move(incoming);
        } else if (lastBackgroundRequest && Clock::now() - *lastBackgroundRequest < std::chrono::seconds(5)) {
            blocked("RATE_LIMITED");
        } else {
            submitRequest(std::move(incoming), true);
        }
    }

    void bridge(std::string_view method, const Json& args, int64_t number, bool visible) {
        auto bad = [&] { policyDiag("bridge-rejected", method); };
        auto activated = [&] {
            if (useActivation(number, visible)) return true;
            policyDiag("activation-blocked", method);
            return false;
        };
        if (method == "ready" && args.empty()) {
            action({});
        } else if (oneOf(method, {"openLink", "openTelegramLink"}) && args.size() == 1 && args["url"].isString()) {
            const auto target = method == "openLink" ? urls::Link::External : urls::Link::Telegram;
            auto link = urls::navigation(args["url"].asString(), target);
            if (!link) return bad();
            if (activated()) action({target == urls::Link::External ? Action::Kind::OpenLink
                                                                    : Action::Kind::OpenTelegramLink,
                                     std::move(link->url), {}, {}});
        } else if (method == "copyText" && args.size() == 1 && args["text"].isString()) {
            const auto& text = args["text"].asString();
            if (text.empty() || utf16Units(text) > 256 || !wellFormed(text)) return bad();
            if (activated() && host.onCopyText) host.onCopyText(text);
        } else if (method == "haptic" && args.size() == 2 && args["value"].isString()) {
            const auto& category = args["haptic"].asString();
            const auto& style = args["value"].asString();
            if (!((category == "impact" && oneOf(style, {"light", "medium", "heavy", "rigid", "soft"})) ||
                  (category == "notification" && oneOf(style, {"error", "success", "warning"})) ||
                  (category == "selection" && style.empty()))) return bad();
            const auto now = Clock::now();
            if (allowHaptic(now, visible)) action({Action::Kind::Haptic, {}, category, style});
            else if (!hapticDiagAt || now - *hapticDiagAt >= std::chrono::seconds(1)) {
                hapticDiagAt = now;
                policyDiag("haptic-limit", category);
            }
        } else if (method == "setResizeAnchor" && args.size() == 1 && oneOf(args["edge"].asString(), {"top", "bottom"})) {
            diagnostics.resizeAnchor = args["edge"].asString();
            notify();
        } else if (method == "watchPublicData" && args.size() == 1 && args["on"].isBool()) {
            if (options.context.preview) return policyDiag("preview-blocked", method);
            const bool on = args["on"].asBool();
            if (diagnostics.watchesPublicData != on) {
                diagnostics.watchesPublicData = on;
                if (host.onWatchPublicData) host.onWatchPublicData(on);
                notify();
            }
        } else if (method == "request" && args.size() == 3 && args["data"].isString() && args["wallet"].isBool()) {
            const int64_t id = validation::positiveId(args["id"]);
            const auto& data = args["data"].asString();
            if (!id || data.empty() || data.size() > 64 || !wellFormed(data)) {
                bad();
                if (id) reject(id, "FAILED");
                return;
            }
            request({id, {data, args["wallet"].asBool()}}, number, visible);
        } else if (oneOf(method, {"sendTransaction", "signData"})) {
            // PLAN §3: this host does not provide wallet windows.
            const int64_t id = validation::positiveId(args["id"]);
            if (id) reject(id, "NOT_ALLOWED", options.context.wallet ? "unsupported" : "disabled");
            else bad();
        } else {
            bad();
        }
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
            if (!value || !value->isArray() || value->size() != 2 || !(*value)[0].isString() || !(*value)[1].isObject()) {
                post([](Impl& h) { h.policyDiag("bridge-rejected", "invalid method or arguments"); });
            } else {
                post([method = (*value)[0].asString(), args = (*value)[1],
                      gesture = engine.currentGesture(), visible = engine.visibleNow()](Impl& h) {
                    h.bridge(method, args, gesture, visible);
                });
            }
        } else if (kind == "supports") {
            const double start = cpuMs();
            const auto value = Json::parse(arg);
            bool supported = false;
            if (value && value->isObject()) {
                const auto& condition = (*value)["condition"];
                const auto& property = (*value)["property"];
                const auto& declaration = (*value)["value"];
                if (value->size() == 1 && validation::boundedString(condition))
                    supported = validator.supportsCondition(condition.asString());
                else if (value->size() == 2 && validation::boundedString(property) && validation::boundedString(declaration))
                    supported = validator.supports(property.asString(), declaration.asString());
            }
            validatorMs += cpuMs() - start;
            return Result::string(supported ? "1" : "0");
        } else if (kind == "intl") {
            const auto request = Json::parse(arg);
            if (!request || !request->isObject()) return Result::string("");
            const auto& provider = options.intl ? *options.intl : defaultIntlProvider();
            return Result::string(provider.format(*request).value_or(""));
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
        const bool appeared = visible && !h.options.engine.visible;
        h.options.engine.visible = visible;
        h.engine.setVisible(visible);
        if (appeared) h.lastBackgroundRequest.reset();
        if (appeared && h.heldRequest) {
            auto request = std::move(*h.heldRequest);
            h.heldRequest.reset();
            if (h.pendingRequest) h.reject(request.id, "BUSY");
            else h.submitRequest(std::move(request), true);
        }
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
