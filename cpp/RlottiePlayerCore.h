// Chunk 1.2 — a single-thread-use façade over rlottie.
//
// This is the ONLY core type that talks to rlottie. rlottie.h is included from
// RlottiePlayerCore.cpp only; the header forward-declares rlottie::Animation so
// that platform adapters including this header do NOT pull in rlottie.
//
// Threading: ALL methods are [worker]. This class is NOT internally
// synchronized — RenderCoordinator (Chunk 1.5) serializes every call onto the
// per-view render worker, which is what makes "the same animation is never
// rendered and mutated concurrently" hold (CLAUDE.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "AnimationMetadata.h"
#include "PlayerError.h"

namespace rlottie {
class Animation;
}

namespace rnrlottie {

class RlottiePlayerCore final {
public:
    struct LoadResult {
        bool success = false;
        AnimationMetadata metadata;
        PlayerError error;
    };

    RlottiePlayerCore();
    ~RlottiePlayerCore();  // destroys the rlottie::Animation (on the worker)
    RlottiePlayerCore(const RlottiePlayerCore&) = delete;
    RlottiePlayerCore& operator=(const RlottiePlayerCore&) = delete;

    // Replace the current animation from a file or raw JSON. On failure the
    // previous animation is dropped and isLoaded() becomes false.
    LoadResult loadFromFile(const std::string& path, bool useModelCache);
    LoadResult loadFromData(std::string json, const std::string& cacheKey,
                            const std::string& resourcePath, bool useModelCache);

    // Renders `frame` into caller-owned premultiplied-ARGB32 memory (byte order
    // B,G,R,A — see cpp/PixelFormat.h). Never allocates. Preconditions:
    // pixels != null, width>0, height>0, bytesPerLine >= width*4. `frame` is
    // clamped to [0, totalFrames-1]. Returns false and sets lastError() on a
    // precondition violation or when no source is loaded.
    bool renderFrame(std::size_t frame, std::uint32_t* pixels,
                     std::size_t width, std::size_t height,
                     std::size_t bytesPerLine, bool preserveAspectRatio);

    // Overrides a fill color by key path. Colors are 0..1. No-op if not loaded.
    // Extended to stroke/opacity in Phase 6.
    void setColor(const std::string& keyPath, float red, float green, float blue);

    AnimationMetadata metadata() const;  // valid after a successful load
    std::size_t frameForProgress(double progress) const;  // clamps progress to [0,1]
    bool isLoaded() const;
    const PlayerError& lastError() const;

private:
    LoadResult finishLoad(std::unique_ptr<rlottie::Animation> anim);

    std::unique_ptr<rlottie::Animation> animation_;  // owns; sole reference
    AnimationMetadata metadata_;
    PlayerError lastError_;
};

}  // namespace rnrlottie
