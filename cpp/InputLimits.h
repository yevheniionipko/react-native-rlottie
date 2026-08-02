// Chunk 1.6 — the input-limit policy consumed by the source resolvers and the
// core. Lottie input is untrusted (parsed by native C++), so every size is
// bounded (plan §16). These are the defaults; the platform module may override
// them via the global config (Chunk 2.4 / 3.4).
//
// This header provides the policy struct + pure validation primitives. Wiring
// them through the load/render path end-to-end is Chunk 5.1.
#pragma once

#include <cstddef>

#include "PlayerError.h"

namespace rnrlottie {

struct InputLimits {
    std::size_t maxJsonBytes = 16u * 1024u * 1024u;   // 16 MiB
    std::size_t maxPixels = 4096u * 4096u;            // render-area cap
    std::size_t maxFrames = 100000u;                  // pathological frame count guard
    std::size_t maxExternalAssets = 64u;              // external raster assets
    std::size_t maxExternalBytes = 32u * 1024u * 1024u;
};

// Returns a truthy PlayerError (SourceTooLarge) when the JSON exceeds the byte
// limit, else a falsy (None) error.
inline PlayerError checkJsonSize(std::size_t jsonBytes, const InputLimits& limits) {
    if (jsonBytes > limits.maxJsonBytes) {
        return PlayerError{PlayerErrorCode::SourceTooLarge,
                           "animation JSON exceeds the configured byte limit", 0};
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

}  // namespace rnrlottie
