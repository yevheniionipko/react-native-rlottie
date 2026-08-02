// Canonical, cross-platform error-code strings emitted to JS.
//
// Both platform adapters (ios/RNRlottiePlayer.mm and android/.../RlottieJni.cpp)
// map PlayerErrorCode through this ONE function, so the strings are byte-for-byte
// identical on iOS and Android — a parity requirement (plan §20/§21).
//
// Phase 4 note: the TS RlottieErrorEvent union (Chunk 4.1) must include every
// string returned here. Relative to plan §11 that means adding INVALID_DIMENSIONS
// and RELEASED (or remapping them there) — decide once, in the TS layer, and keep
// this function as the single source of truth the native layers agree on.
#pragma once

#include "PlayerError.h"

namespace rnrlottie {

inline const char* errorCodeString(PlayerErrorCode code) {
    switch (code) {
        case PlayerErrorCode::None: return "NONE";
        case PlayerErrorCode::InvalidSource: return "INVALID_SOURCE";
        case PlayerErrorCode::SourceNotFound: return "SOURCE_NOT_FOUND";
        case PlayerErrorCode::SourceTooLarge: return "SOURCE_TOO_LARGE";
        case PlayerErrorCode::ParseFailed: return "PARSE_FAILED";
        case PlayerErrorCode::InvalidDimensions: return "INVALID_DIMENSIONS";
        case PlayerErrorCode::AllocationFailed: return "ALLOCATION_FAILED";
        case PlayerErrorCode::RenderFailed: return "RENDER_FAILED";
        case PlayerErrorCode::Released: return "RELEASED";
    }
    return "RENDER_FAILED";
}

}  // namespace rnrlottie
