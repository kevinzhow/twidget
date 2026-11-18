#pragma once

#include <quickjs.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace twcard {

struct CompiledBundle {
    std::vector<uint8_t> bytes;
    std::string error;
};

/// S0: compile trusted source in a temporary runtime and retain debug line tables.
inline CompiledBundle compileTrustedBundle(const std::string& source) {
    const std::unique_ptr<JSRuntime, decltype(&JS_FreeRuntime)> runtime(JS_NewRuntime(), JS_FreeRuntime);
    if (!runtime) return {{}, "cannot create bundle compiler runtime"};
    const std::unique_ptr<JSContext, decltype(&JS_FreeContext)> context(JS_NewContext(runtime.get()), JS_FreeContext);
    if (!context) return {{}, "cannot create bundle compiler context"};
    auto* ctx = context.get();

    const auto error = [ctx](const char* fallback) -> CompiledBundle {
        auto exception = JS_GetException(ctx);
        const char* message = JS_ToCString(ctx, exception);
        std::string detail = message ? message : fallback;
        if (message) JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, exception);
        return {{}, std::move(detail)};
    };
    auto fn = JS_Eval(ctx, source.data(), source.size(), "twcard.js",
                      JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(fn)) return error("cannot compile bundle");

    size_t size = 0;
    const auto freeBytes = [ctx](uint8_t* bytes) { js_free(ctx, bytes); };
    const std::unique_ptr<uint8_t, decltype(freeBytes)> bytes(
        JS_WriteObject(ctx, &size, fn, JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_STRIP_SOURCE), freeBytes);
    JS_FreeValue(ctx, fn);
    if (!bytes) return error("cannot serialize bundle");
    return {{bytes.get(), bytes.get() + size}, {}};
}

} // namespace twcard
