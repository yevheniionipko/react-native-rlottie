// Chunk 1.6 — the input-limit policy consumed by the source resolvers and the
// core. Lottie input is untrusted (parsed by native C++), so every size is
// bounded (plan §16). These are the defaults; the platform module may override
// them via the global config (Chunk 2.4 / 3.4).
//
// This header provides the policy struct + pure validation primitives.
// Chunk 5.1 wires them through the load/render path end-to-end (see
// RlottiePlayerCore::loadFromData/loadFromFile and FrameBuffer::resize) and
// adds currentInputLimits()/setInputLimits() below: a process-global,
// runtime-configurable seam so a platform's "configure" call (plan §16
// "configurable byte limit") actually changes what the core enforces, instead
// of every call site reading a fresh compile-time InputLimits{}.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

#include "PlayerError.h"

namespace rnrlottie {

struct InputLimits {
    std::size_t maxJsonBytes = 16u * 1024u * 1024u;   // 16 MiB
    std::size_t maxPixels = 4096u * 4096u;            // render-area cap
    std::size_t maxFrames = 100000u;                  // pathological frame count guard
    std::size_t maxExternalAssets = 64u;              // external raster assets
    std::size_t maxExternalBytes = 32u * 1024u * 1024u;
    // Maximum JSON nesting depth. See checkJsonDepth() below for why a byte
    // limit alone does NOT bound this. Real Lottie files nest a few dozen deep
    // at most (shape groups inside groups), so 512 is far above any legitimate
    // animation while staying orders of magnitude below what threatens a stack.
    std::size_t maxJsonDepth = 512u;
};

// Deliberately header-only (inline, function-local statics) rather than a
// separate InputLimits.cpp: both android/src/main/cpp/CMakeLists.txt and
// tests/cpp/CMakeLists.txt enumerate cpp/*.cpp explicitly rather than globbing,
// and adding a new .cpp there is out of scope for this chunk (owned by the
// Android agent). A C++17 inline function's function-local static is one
// instance for the whole program regardless of how many TUs include this
// header, so this is exactly as global/thread-safe as a .cpp-backed singleton
// — just without the extra build-file wiring.
namespace detail {

inline std::mutex& inputLimitsMutex() {
    static std::mutex m;
    return m;
}

inline InputLimits& inputLimitsStorage() {
    static InputLimits limits{};
    return limits;
}

}  // namespace detail

// The process-global, runtime-configurable InputLimits. Defaults to
// `InputLimits{}` until a caller (typically the platform global-config module,
// Chunk 2.4/3.4) calls setInputLimits(). Thread-safe (mutex-guarded); safe to
// call from [any] thread, including concurrently with a render worker's load
// path.
//
// This is the ONE plain C++ getter every layer — core, iOS, Android — must
// read instead of constructing `InputLimits{}` locally, so "configurable byte
// limit" (plan §16) is not just a documented intent but an actual, single
// source of truth. In particular this should replace the Android resolver's
// hardcoded 16 MiB constant and the several `rnrlottie::InputLimits{}`
// default-constructions in ios/RNRlottieSourceResolver.mm / RNRlottieView.mm
// (both out of scope for this agent — see CLAUDE.md file ownership).
inline InputLimits currentInputLimits() {
    std::lock_guard<std::mutex> lk(detail::inputLimitsMutex());
    return detail::inputLimitsStorage();
}

// Configure the process-global InputLimits used by currentInputLimits() from
// this point forward. Does not retroactively affect an animation already
// loaded under the previous limits.
inline void setInputLimits(const InputLimits& limits) {
    std::lock_guard<std::mutex> lk(detail::inputLimitsMutex());
    detail::inputLimitsStorage() = limits;
}

// Returns a truthy PlayerError (SourceTooLarge) when the JSON exceeds the byte
// limit, else a falsy (None) error.
inline PlayerError checkJsonSize(std::size_t jsonBytes, const InputLimits& limits) {
    if (jsonBytes > limits.maxJsonBytes) {
        return PlayerError{PlayerErrorCode::SourceTooLarge,
                           "animation JSON exceeds the configured byte limit", 0};
    }
    return PlayerError{};
}

// Returns a truthy PlayerError (ParseFailed) when the JSON nests deeper than
// the configured limit.
//
// THIS IS A CRASH GUARD, NOT A POLICY NICETY. rlottie's vendored rapidjson
// parses with the default recursive-descent flags (see lottieparser.cpp's
// `kParseDefaultFlags | kParseInsituFlag` — no kParseIterativeFlag), so parsing
// recurses once per nesting level and deep input exhausts the native stack:
// the process dies with SIGSEGV, which no try/catch or typed error can recover.
//
// The byte limit does NOT bound this, which is the trap. The nesting token in a
// Lottie shape tree (`{"ty":"gr","it":[`) is ~18 bytes, so the 16 MiB default
// still permits ~930k levels; a bare `[[[[…` is one byte per level. Measured on
// a normal 8 MiB main-thread stack, depth 20,000 parsed fine and depth 100,000
// (only ~1.9 MB of JSON, ~11% of the byte limit) reproducibly SIGSEGV'd. On a
// secondary thread — which is exactly where RenderCoordinator parses — stacks
// are typically 512 KB, so the real threshold is far lower still.
//
// Scanning is a single pass and string-aware: brackets inside JSON string
// literals are not structure, and a `\"` escape inside a string must not be
// read as the closing quote. It deliberately does NOT validate the JSON —
// rlottie still does that — it only refuses to hand the parser something that
// would kill the process before it can report an error.
inline PlayerError checkJsonDepth(const std::string& json, const InputLimits& limits) {
    std::size_t depth = 0;
    bool inString = false;
    bool escaped = false;

    for (const char c : json) {
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        switch (c) {
            case '"':
                inString = true;
                break;
            case '[':
            case '{':
                if (++depth > limits.maxJsonDepth) {
                    return PlayerError{PlayerErrorCode::ParseFailed,
                                       "animation JSON nests deeper than the configured limit",
                                       0};
                }
                break;
            case ']':
            case '}':
                if (depth > 0) {
                    --depth;
                }
                break;
            default:
                break;
        }
    }
    return PlayerError{};
}

// Returns a truthy PlayerError (SourceTooLarge) when the animation declares more
// frames than allowed.
inline PlayerError checkFrameCount(std::size_t totalFrames, const InputLimits& limits) {
    if (totalFrames > limits.maxFrames) {
        return PlayerError{PlayerErrorCode::SourceTooLarge,
                           "animation frame count exceeds the configured limit", 0};
    }
    return PlayerError{};
}

// Overflow-checked pixel-area validation, shared by every dimension check in
// the core: the render-surface size (FrameBuffer::resize) AND the parsed
// animation's intrinsic composition size (RlottiePlayerCore, Chunk 5.1). A
// single implementation means the two call sites cannot silently drift apart
// on what "invalid" means. Rejects zero, an axis outside 32-bit range (rlottie
// surfaces are addressed with size_t but a sane render target never needs
// more; this also keeps `width*height` and downstream `*4` byte-size math from
// being anywhere near overflow), a width*height multiplication overflow, and a
// pixel count over `maxPixels`.
inline PlayerError checkDimensions(std::size_t width, std::size_t height,
                                   std::size_t maxPixels) {
    if (width == 0 || height == 0) {
        return PlayerError{PlayerErrorCode::InvalidDimensions, "zero width or height", 0};
    }
    constexpr std::size_t kMaxAxis = std::numeric_limits<std::uint32_t>::max();
    if (width > kMaxAxis || height > kMaxAxis) {
        return PlayerError{PlayerErrorCode::InvalidDimensions,
                           "dimension exceeds 32-bit range", 0};
    }
    if (width > std::numeric_limits<std::size_t>::max() / height) {
        return PlayerError{PlayerErrorCode::InvalidDimensions, "pixel count overflow", 0};
    }
    if (width * height > maxPixels) {
        return PlayerError{PlayerErrorCode::InvalidDimensions,
                           "pixel count exceeds configured maximum", 0};
    }
    return PlayerError{};
}

inline PlayerError checkDimensions(std::size_t width, std::size_t height,
                                   const InputLimits& limits) {
    return checkDimensions(width, height, limits.maxPixels);
}

}  // namespace rnrlottie
