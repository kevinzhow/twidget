// twcard::Validator (SPEC S9): every message out of the card world is checked here before it reaches the renderer or
// the app (I3). The validator keeps its own model of the card's document (S9.2) and never trusts anything else the
// card world claims: each op is checked against the model and profile 1 (twcard/src/profile.h), each URL against S9.4,
// each piece of CSS by the sanitizer (S9.5), and the counted caps of S9.6 are kept per document and per task.
//
// What reaches the renderer is only what the model says the renderer should know: a node under a dropped or
// unrendered element is tracked but not sent, and sent (creations re-emitted) once it moves under a rendered parent.
//
// One per card, used on the card's JS thread only (S10.6).
#pragma once

#include "twidget/json.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace twcard {

class CssSanitizer;

struct ValidatorOptions {
    /// S1.6 `policy`: `compat` (tests and measurements only) has the larger caps of S9.6 and stops nothing.
    bool compat = false;
};

class Validator {
public:
    using Diag = std::function<void(std::string_view kind, std::string_view detail)>;

    Validator(ValidatorOptions options, Diag diag);
    ~Validator();
    Validator(const Validator&) = delete;
    Validator& operator=(const Validator&) = delete;

    /// The result of one `ops` batch (S2.4, S9.2-S9.6).
    struct Batch {
        twidget::Json ops = twidget::Json::array();  // what the renderer gets (`{type:"ops", ops}`); may be empty
        size_t canvasCommands = 0;                     // in `ops`, for the cost model (S1.7)
        std::optional<std::string> stop;               // a counted cap was passed: the card stops with this reason
    };

    /// The start of a task: DOM changes are counted per task (S9.6).
    void beginTask();
    /// host('ops'): one batch. Once a cap stopped the card, every later batch is refused.
    Batch ops(std::string_view batchJson);

    /// host('anim') (S9.7): the records for the renderer's `{type:"anim", now, anims}`; nullopt when none pass.
    std::optional<twidget::Json> anim(std::string_view json);
    /// host('listen') (S3.1, S9.7): the event types that may be forwarded; nullopt for a malformed message.
    std::optional<twidget::Json> listen(std::string_view json);
    /// host('pd') (S3.3, S9.7): the `"<id>:<type>"` keys of known rendered nodes.
    std::optional<twidget::Json> preventDefault(std::string_view json);
    /// host('query') (S5, S9.7): the checked query, rebuilt from its parsed fields; nullopt (with `error` set) when
    /// it is refused — the card then gets `{"error": error}`.
    std::optional<twidget::Json> query(std::string_view json, std::string& error);

    /// The rendered element `id` (the renderer knows it and it is in the document).
    bool renderedElement(int64_t id) const;
    /// Nodes in the document (S9.6) and every node the validator tracks.
    size_t documentNodes() const;
    size_t trackedNodes() const;
    bool stopped() const { return stopped_.has_value(); }

    /// `CSS.supports` / `@supports` (S9.1: one evaluator for both).
    bool supports(std::string_view property, std::string_view value) const;
    bool supportsCondition(std::string_view condition) const;

    /// The caps of S9.6 (strict, or compat).
    struct Caps {
        size_t nodes = 2000;
        size_t textValue = 32 * 1024;      // UTF-16 units
        size_t documentText = 48 * 1024;   // UTF-16 units
        size_t attribute = 8 * 1024;       // UTF-16 units
        size_t domChanges = 10000;         // per task
        size_t canvasCommands = 4000;      // per canvas op
        size_t cssRules = 400;
        size_t keyframes = 32;
        uint64_t canvasPixels = 2048ull * 2048ull;
        size_t complexText = 8192;         // UTF-16 units of complex-script text per document (refused, not stopped)
        size_t liveAnimations = 64;        // script animations (S9.7)
        size_t trackedNodes = 50000;       // ours: what the validator keeps of detached nodes, see validator.cpp
        bool stops = true;
    };
    const Caps& caps() const { return caps_; }

private:
    struct Impl;
    Caps caps_;
    Diag diag_;
    std::unique_ptr<CssSanitizer> css_;
    std::unique_ptr<Impl> impl_;
    std::optional<std::string> stopped_;
};

} // namespace twcard
