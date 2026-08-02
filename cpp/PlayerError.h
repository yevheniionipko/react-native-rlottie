// Chunk 1.1 — typed error results for the shared core.
//
// The core NEVER throws across a JNI / Objective-C++ boundary (plan §17); it
// returns these instead. Adapters map `code` to the React error event and, in
// dev builds, may attach a native diagnostic to `message`.
//
// Threading: plain value type, usable on [any] thread.
#pragma once

#include <cstdint>
#include <string>

namespace rnrlottie {

// Stable, machine-readable error codes. Mirrored by the TS RlottieErrorEvent
// union (plan §11) and the Android/iOS error mapping.
enum class PlayerErrorCode {
    None,
    InvalidSource,
    SourceNotFound,
    SourceTooLarge,
    ParseFailed,
    InvalidDimensions,
    AllocationFailed,
    RenderFailed,
    Released,
};

struct PlayerError {
    PlayerErrorCode code = PlayerErrorCode::None;
    std::string message;              // developer-readable
    std::uint64_t generation = 0;     // source generation this error belongs to

    // True when this represents a real error (code != None).
    explicit operator bool() const { return code != PlayerErrorCode::None; }
};

}  // namespace rnrlottie
