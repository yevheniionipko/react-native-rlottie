// Chunk 1.2 — RlottiePlayerCore implementation. The only core TU that includes
// rlottie.h.
#include "RlottiePlayerCore.h"

#include <limits>
#include <tuple>
#include <utility>

#include <rlottie.h>

namespace rnrlottie {
namespace {

AnimationMetadata extractMetadata(rlottie::Animation& anim) {
    AnimationMetadata md;
    anim.size(md.width, md.height);
    md.frameRate = anim.frameRate();
    md.totalFrames = anim.totalFrame();
    md.duration = anim.duration();
    // rlottie::MarkerList = std::vector<std::tuple<std::string, int, int>>.
    for (const auto& m : anim.markers()) {
        const int start = std::get<1>(m);
        const int end = std::get<2>(m);
        Marker marker;
        marker.name = std::get<0>(m);
        marker.startFrame = start > 0 ? static_cast<std::size_t>(start) : 0;
        marker.endFrame = end > 0 ? static_cast<std::size_t>(end) : 0;
        md.markers.push_back(std::move(marker));
    }
    return md;
}

}  // namespace

RlottiePlayerCore::RlottiePlayerCore() = default;
RlottiePlayerCore::~RlottiePlayerCore() = default;

RlottiePlayerCore::LoadResult RlottiePlayerCore::finishLoad(
    std::unique_ptr<rlottie::Animation> anim) {
    LoadResult result;
    if (!anim) {
        animation_.reset();
        metadata_ = AnimationMetadata{};
        lastError_ = PlayerError{PlayerErrorCode::ParseFailed,
                                 "rlottie failed to parse the animation source", 0};
        result.success = false;
        result.error = lastError_;
        return result;
    }
    metadata_ = extractMetadata(*anim);
    animation_ = std::move(anim);
    lastError_ = PlayerError{};
    result.success = true;
    result.metadata = metadata_;
    return result;
}

RlottiePlayerCore::LoadResult RlottiePlayerCore::loadFromFile(const std::string& path,
                                                              bool useModelCache) {
    return finishLoad(rlottie::Animation::loadFromFile(path, useModelCache));
}

RlottiePlayerCore::LoadResult RlottiePlayerCore::loadFromData(
    std::string json, const std::string& cacheKey, const std::string& resourcePath,
    bool useModelCache) {
    return finishLoad(rlottie::Animation::loadFromData(std::move(json), cacheKey,
                                                       resourcePath, useModelCache));
}

bool RlottiePlayerCore::renderFrame(std::size_t frame, std::uint32_t* pixels,
                                    std::size_t width, std::size_t height,
                                    std::size_t bytesPerLine, bool preserveAspectRatio) {
    if (!animation_) {
        lastError_ = PlayerError{PlayerErrorCode::RenderFailed,
                                 "renderFrame called before a source was loaded", 0};
        return false;
    }
    if (pixels == nullptr || width == 0 || height == 0) {
        lastError_ = PlayerError{PlayerErrorCode::InvalidDimensions,
                                 "null buffer or zero dimensions", 0};
        return false;
    }
    if (width > std::numeric_limits<std::size_t>::max() / 4 || bytesPerLine < width * 4) {
        lastError_ = PlayerError{PlayerErrorCode::InvalidDimensions,
                                 "bytesPerLine smaller than width*4 (or width overflow)", 0};
        return false;
    }

    // Clamp to a valid frame index to avoid feeding rlottie an out-of-range frame.
    if (metadata_.totalFrames > 0 && frame >= metadata_.totalFrames) {
        frame = metadata_.totalFrames - 1;
    }

    rlottie::Surface surface(pixels, width, height, bytesPerLine);
    animation_->renderSync(frame, surface, preserveAspectRatio);
    lastError_ = PlayerError{};
    return true;
}

void RlottiePlayerCore::setColor(const std::string& keyPath, float red, float green,
                                 float blue) {
    if (!animation_) {
        return;
    }
    animation_->setValue<rlottie::Property::FillColor>(keyPath,
                                                       rlottie::Color(red, green, blue));
}

AnimationMetadata RlottiePlayerCore::metadata() const { return metadata_; }

std::size_t RlottiePlayerCore::frameForProgress(double progress) const {
    if (!animation_) {
        return 0;
    }
    if (progress < 0.0) {
        progress = 0.0;
    } else if (progress > 1.0) {
        progress = 1.0;
    }
    // unique_ptr const-ness does not propagate to the pointee, so calling the
    // non-const frameAtPos() from this const method is well-formed.
    return animation_->frameAtPos(progress);
}

bool RlottiePlayerCore::isLoaded() const { return animation_ != nullptr; }

const PlayerError& RlottiePlayerCore::lastError() const { return lastError_; }

}  // namespace rnrlottie
