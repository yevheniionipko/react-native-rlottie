// Chunk 1.2 — RlottiePlayerCore implementation. The only core TU that includes
// rlottie.h.
//
// Chunk 5.1 wires InputLimits enforcement into the load path here (the core,
// not just the platform resolvers — plan §16's "untrusted input" boundary is
// the native parser, and a resolver check is bypassable by anything that talks
// to this class directly, e.g. a future Fabric adapter or a test). Two of the
// three checks (JSON byte size, file byte size) run BEFORE rlottie ever sees
// the bytes. The other two (frame count, composition dimensions) are only
// knowable from rlottie's own parsed output, so they run immediately after
// finishLoad() extracts metadata and BEFORE the load is reported as a success:
// on rejection animation_ is dropped, exactly as if the parse had failed, so a
// pathological animation is never exposed to render() or handed further up.
#include "RlottiePlayerCore.h"

#include <sys/stat.h>

#include <limits>
#include <tuple>
#include <utility>

#include <rlottie.h>

#include "CacheKey.h"
#include "RlottieVersion.h"

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

// Helper for the pre-parse rejection paths: leaves the core in the same
// "no source loaded" state a parse failure would, so callers cannot observe a
// difference between "rlottie rejected this" and "we rejected this before
// rlottie saw it".
RlottiePlayerCore::LoadResult rejected(const PlayerError& err) {
    RlottiePlayerCore::LoadResult result;
    result.success = false;
    result.error = err;
    return result;
}

}  // namespace

RlottiePlayerCore::RlottiePlayerCore() = default;
RlottiePlayerCore::~RlottiePlayerCore() = default;

RlottiePlayerCore::LoadResult RlottiePlayerCore::finishLoad(
    std::unique_ptr<rlottie::Animation> anim, const InputLimits& limits) {
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

    AnimationMetadata md = extractMetadata(*anim);

    // Post-parse limit enforcement (Chunk 5.1): frame count and the intrinsic
    // composition size are only known once rlottie has parsed the document, so
    // this is the earliest point they can be checked. On rejection the parsed
    // animation is discarded (never assigned to animation_), so it can never
    // reach renderFrame() or be reported as loaded.
    PlayerError limitErr = checkFrameCount(md.totalFrames, limits);
    if (!limitErr) {
        limitErr = checkDimensions(md.width, md.height, limits);
    }
    if (limitErr) {
        animation_.reset();
        metadata_ = AnimationMetadata{};
        lastError_ = limitErr;
        result.success = false;
        result.error = limitErr;
        return result;
    }

    metadata_ = std::move(md);
    animation_ = std::move(anim);
    lastError_ = PlayerError{};
    result.success = true;
    result.metadata = metadata_;
    return result;
}

RlottiePlayerCore::LoadResult RlottiePlayerCore::loadFromFile(const std::string& path,
                                                              bool useModelCache) {
    const InputLimits limits = currentInputLimits();

    // Chunk 5.1: reject an oversize file before rlottie ever opens it. `stat`
    // (not opening + reading) keeps this cheap and matches the check
    // ios/RNRlottieSourceResolver.mm already does at resolve time — this is
    // the core's OWN gate, independent of whichever platform resolver ran
    // first, per plan §16's "untrusted input" boundary being the native parser.
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        PlayerError err{PlayerErrorCode::SourceNotFound,
                        "animation file does not exist or is not accessible", 0};
        animation_.reset();
        metadata_ = AnimationMetadata{};
        lastError_ = err;
        return rejected(err);
    }
    if (st.st_size < 0 ||
        static_cast<std::uint64_t>(st.st_size) > static_cast<std::uint64_t>(limits.maxJsonBytes)) {
        PlayerError err{PlayerErrorCode::SourceTooLarge,
                        "animation file exceeds the configured byte limit", 0};
        animation_.reset();
        metadata_ = AnimationMetadata{};
        lastError_ = err;
        return rejected(err);
    }

    return finishLoad(rlottie::Animation::loadFromFile(path, useModelCache), limits);
}

RlottiePlayerCore::LoadResult RlottiePlayerCore::loadFromData(
    std::string json, const std::string& cacheKey, const std::string& resourcePath,
    bool useModelCache) {
    const InputLimits limits = currentInputLimits();

    // Chunk 5.1: reject oversize JSON before rlottie ever tokenizes it.
    const PlayerError sizeErr = checkJsonSize(json.size(), limits);
    if (sizeErr) {
        animation_.reset();
        metadata_ = AnimationMetadata{};
        lastError_ = sizeErr;
        return rejected(sizeErr);
    }

    // Chunk 5.1: reject pathologically nested JSON before rlottie's recursive
    // parser sees it. Unlike every other check here, failing this one is not a
    // recoverable error — the parser would exhaust the native stack and take the
    // process down. See checkJsonDepth() for the measured thresholds.
    const PlayerError depthErr = checkJsonDepth(json, limits);
    if (depthErr) {
        animation_.reset();
        metadata_ = AnimationMetadata{};
        lastError_ = depthErr;
        return rejected(depthErr);
    }

    // Chunk 5.3: compose the deterministic key this load is cached under. See
    // CacheKey.h for why this recomputes sha256 over `json` itself rather than
    // trusting the caller-supplied `cacheKey`.
    const std::string finalKey = composeCacheKey(json, cacheKey, kRlottieCommit, resourcePath);

    return finishLoad(
        rlottie::Animation::loadFromData(std::move(json), finalKey, resourcePath, useModelCache),
        limits);
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

namespace {
// rlottie has no documented valid range for Property::StrokeWidth (unlike the
// opacity properties, which are explicitly [0,100] in rlottie.h); zero/negative
// widths have no sane visual meaning, so clamp to a small strictly-positive
// floor rather than accepting them verbatim.
constexpr float kMinStrokeWidth = 0.01f;
}  // namespace

void RlottiePlayerCore::setOpacity(const std::string& keyPath, float opacity) {
    if (!animation_) {
        return;
    }
    // `!(opacity >= 0.0f)` also catches NaN (every comparison with NaN is
    // false, so the negation is true), unlike `opacity < 0.0f` which would let
    // NaN through unclamped.
    if (!(opacity >= 0.0f)) {
        opacity = 0.0f;
    } else if (opacity > 1.0f) {
        opacity = 1.0f;
    }
    // rlottie::Property::FillOpacity: "Opacity property of Fill object, value
    // type is float [0..100]" (rlottie.h). This targets fill opacity only, the
    // same scope setColor already has (FillColor, not StrokeColor) — there is
    // no single rlottie property that scales overall (fill+stroke) opacity, so
    // exposing one setOpacity() means picking a side. Fill was chosen for
    // parity with setColor. LIMITATION: an override on a stroke-only shape
    // (fill-less) has no visible effect; a future setStrokeOpacity() would be
    // a separate, additive API if that gap needs closing.
    animation_->setValue<rlottie::Property::FillOpacity>(keyPath, opacity * 100.0f);
}

void RlottiePlayerCore::setStrokeWidth(const std::string& keyPath, float width) {
    if (!animation_) {
        return;
    }
    // Same NaN-safe clamp shape as setOpacity: `!(width > kMinStrokeWidth)` is
    // true for NaN, negatives, zero, and anything at/below the floor.
    if (!(width > kMinStrokeWidth)) {
        width = kMinStrokeWidth;
    }
    animation_->setValue<rlottie::Property::StrokeWidth>(keyPath, width);
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
