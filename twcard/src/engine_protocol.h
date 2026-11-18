// The engine and runtime bundle protocol (SPEC S1.2 and S1.3a).
#pragma once

#include <cstddef>
#include <string_view>

namespace twcard::protocol {

// Globals the engine defines before the bundle runs and deletes after it (S1.2.2).
inline constexpr const char* kHostGlobal = "host";
inline constexpr const char* kHostBinaryGlobal = "hostBinary";
// The bundle's dispatch function.
inline constexpr const char* kDispatchGlobal = "dispatch";

// Dispatch kinds (engine -> runtime).
inline constexpr const char* kBoot = "boot";
inline constexpr const char* kScript = "script";
inline constexpr const char* kScriptDone = "scriptDone";
inline constexpr const char* kScriptError = "scriptError";
inline constexpr const char* kLoaded = "loaded";
inline constexpr const char* kEndTick = "endTick";
inline constexpr const char* kTimer = "timer";
inline constexpr const char* kFrame = "frame";
inline constexpr const char* kEvent = "event";
inline constexpr const char* kViewport = "viewport";
inline constexpr const char* kResize = "resize";
inline constexpr const char* kTheme = "theme";
inline constexpr const char* kSafeArea = "safeArea";
inline constexpr const char* kBridge = "bridge";
inline constexpr const char* kVisibility = "visibility";
inline constexpr const char* kRejection = "rejection";

// S1.4.4: at most this many settle rounds per task.
inline constexpr int kMaxSettleRounds = 20;
inline constexpr size_t kMaxErrorBytes = 4096; // S1.3

/// The length cap of a host function argument (S1.3: "generous caps, e.g. 16 MiB for ops, 64 KiB for most others").
inline size_t argument_cap(std::string_view kind) {
    if (kind == "ops") return size_t(16) << 20;
    if (kind == "anim") return size_t(4) << 20;
    if (kind == "pd" || kind == "listen") return size_t(1) << 20;
    if (kind == "storage") return size_t(256) << 10;  // the host refuses past ~100 KB unparsed (S7.4)
    return size_t(64) << 10;
}

} // namespace twcard::protocol
