// Chunk 1.1 — animation metadata extracted after a successful load.
//
// Surfaced to JS via onAnimationLoaded (plan §11). Threading: plain value type,
// usable on [any] thread.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace rnrlottie {

// A named frame segment defined in the Lottie source (used by marker playback,
// Chunk 6.2).
struct Marker {
    std::string name;
    std::size_t startFrame = 0;
    std::size_t endFrame = 0;
};

struct AnimationMetadata {
    std::size_t width = 0;         // intrinsic composition width (px)
    std::size_t height = 0;        // intrinsic composition height (px)
    double frameRate = 0.0;        // frames per second
    std::size_t totalFrames = 0;
    double duration = 0.0;         // seconds
    std::vector<Marker> markers;
};

}  // namespace rnrlottie
