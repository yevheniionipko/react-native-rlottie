// Chunk 1.1 — a resolved, platform-neutral animation source.
//
// This is what the core consumes. It is ALREADY resolved by the platform
// SourceResolver (plan §12): no URIs, no HTTP, no arbitrary filesystem roots.
// The core only ever sees a canonicalized app-private file path or raw JSON.
//
// Threading: plain value type, usable on [any] thread.
#pragma once

#include <string>

namespace rnrlottie {

struct AnimationSource {
    enum class Kind {
        File,   // `path` -> canonicalized, app-private file
        Data,   // `json` -> raw JSON bytes
    };

    Kind kind = Kind::Data;
    std::string path;          // Kind::File
    std::string json;          // Kind::Data
    std::string cacheKey;      // deterministic (Chunk 5.3)
    std::string resourcePath;  // external raster-asset root (validated; may be empty)
    bool useModelCache = true;
};

}  // namespace rnrlottie
