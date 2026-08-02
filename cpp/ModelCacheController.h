// Chunk 1.6 — process-global control of rlottie's library-level model cache.
//
// rlottie caches parsed animation models globally (LOTTIE_CACHE). This is the
// ONLY surface for global cache operations; per-view playback never routes
// through it (plan §15). Exposed to JS via the global module (Chunk 2.4 / 3.4).
//
// Threading: process-global; all methods are [any] and internally serialized.
#pragma once

#include <cstddef>

namespace rnrlottie {

class ModelCacheController {
public:
    static constexpr std::size_t kDefaultCacheSize = 10;

    // Set the maximum number of cached animation models.
    static void setModelCacheSize(std::size_t entries);

    // Flush the cache while preserving the configured size.
    static void clearModelCache();

    // The last configured size (defaults to kDefaultCacheSize).
    static std::size_t currentSize();
};

}  // namespace rnrlottie
