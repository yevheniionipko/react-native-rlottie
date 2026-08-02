// Chunk 1.4 — PlaybackController implementation.
#include "PlaybackController.h"

#include <cmath>
#include <limits>

namespace rnrlottie {

void PlaybackController::queue(PlaybackEvent event) { eventQueue_.push_back(event); }

bool PlaybackController::hasAnimation() const {
    return state_ == PlaybackState::Ready || state_ == PlaybackState::Playing ||
           state_ == PlaybackState::Paused || state_ == PlaybackState::Stopped;
}

void PlaybackController::resolveSegment() {
    const std::size_t last = metadata_.totalFrames > 0 ? metadata_.totalFrames - 1 : 0;
    std::size_t s = config_.startFrame;
    std::size_t e = config_.endFrame == 0 ? last : config_.endFrame;
    if (s > last) s = last;
    if (e > last) e = last;
    if (e < s) e = s;
    segStart_ = s;
    segEnd_ = e;
}

double PlaybackController::finishAtLoop() const {
    if (!config_.loop) return 1.0;                       // play once
    if (config_.repeatCount > 0) return static_cast<double>(config_.repeatCount) + 1.0;
    return std::numeric_limits<double>::infinity();       // infinite loop
}

void PlaybackController::onLoaded(const AnimationMetadata& metadata) {
    metadata_ = metadata;
    resolveSegment();
    speed_ = config_.speed;
    finishedEmitted_ = false;
    lastLoops_ = 0;
    forceRender_ = true;          // present the first frame on load
    hasRendered_ = false;
    if (config_.autoPlay) {
        state_ = PlaybackState::Playing;
        currentPos_ = static_cast<double>(speed_ >= 0 ? segStart_ : segEnd_);
        needsAnchor_ = true;
        queue(PlaybackEvent::Started);
    } else {
        state_ = PlaybackState::Ready;
        currentPos_ = static_cast<double>(segStart_);
    }
    currentFrame_ = static_cast<std::size_t>(currentPos_);
}

void PlaybackController::onLoadFailed(const PlayerError&) { state_ = PlaybackState::Failed; }

void PlaybackController::configure(const PlaybackConfig& config) {
    config_ = config;
    speed_ = config.speed;
    if (hasAnimation()) {
        resolveSegment();
        if (currentPos_ < static_cast<double>(segStart_)) currentPos_ = segStart_;
        if (currentPos_ > static_cast<double>(segEnd_)) currentPos_ = segEnd_;
        currentFrame_ = static_cast<std::size_t>(currentPos_);
        forceRender_ = true;
        if (state_ == PlaybackState::Playing) needsAnchor_ = true;
    }
}

void PlaybackController::play(std::optional<std::size_t> startFrame,
                              std::optional<std::size_t> endFrame) {
    if (startFrame) config_.startFrame = *startFrame;
    if (endFrame) config_.endFrame = *endFrame;

    if (!hasAnimation()) {
        config_.autoPlay = true;  // deferred: play once loaded
        return;
    }
    if (startFrame || endFrame) resolveSegment();

    state_ = PlaybackState::Playing;
    finishedEmitted_ = false;
    currentPos_ = static_cast<double>(speed_ >= 0 ? segStart_ : segEnd_);
    currentFrame_ = static_cast<std::size_t>(currentPos_);
    needsAnchor_ = true;
    lastLoops_ = 0;
    forceRender_ = true;
    queue(PlaybackEvent::Started);
}

void PlaybackController::pause() {
    if (state_ == PlaybackState::Playing) {
        state_ = PlaybackState::Paused;
        queue(PlaybackEvent::Paused);
    }
}

void PlaybackController::resume() {
    if (state_ == PlaybackState::Paused) {
        state_ = PlaybackState::Playing;
        needsAnchor_ = true;  // re-anchor from currentPos_
    }
}

void PlaybackController::stop() {
    if (!hasAnimation()) return;
    state_ = PlaybackState::Stopped;
    currentPos_ = static_cast<double>(segStart_);
    currentFrame_ = segStart_;
    finishedEmitted_ = false;
    forceRender_ = true;
}

void PlaybackController::reset() {
    if (!hasAnimation()) return;
    state_ = PlaybackState::Ready;
    currentPos_ = static_cast<double>(segStart_);
    currentFrame_ = segStart_;
    finishedEmitted_ = false;
    forceRender_ = true;
    eventQueue_.clear();
}

void PlaybackController::seekToFrame(std::size_t frame) {
    if (!hasAnimation()) return;
    if (frame < segStart_) frame = segStart_;
    if (frame > segEnd_) frame = segEnd_;
    currentPos_ = static_cast<double>(frame);
    currentFrame_ = frame;
    finishedEmitted_ = false;
    forceRender_ = true;
    if (state_ == PlaybackState::Playing) needsAnchor_ = true;
}

void PlaybackController::seekToProgress(double progress) {
    if (!hasAnimation()) return;
    if (progress < 0.0) progress = 0.0;
    if (progress > 1.0) progress = 1.0;
    const double span = static_cast<double>(segEnd_ - segStart_);
    const auto frame =
        segStart_ + static_cast<std::size_t>(std::llround(progress * span));
    seekToFrame(frame);
}

void PlaybackController::setSpeed(double speed) {
    config_.speed = speed;
    speed_ = speed;
    if (state_ == PlaybackState::Playing) needsAnchor_ = true;
}

void PlaybackController::advancePlaying(double monotonicNow) {
    const double fps = metadata_.frameRate;
    if (speed_ == 0.0 || fps <= 0.0) {
        return;  // zero speed (or no frame rate) holds the current frame
    }

    const double L = static_cast<double>(segEnd_ - segStart_);
    if (L <= 0.0) {  // single-frame segment: present it, then finish once
        currentFrame_ = segStart_;
        currentPos_ = static_cast<double>(segStart_);
        if (!finishedEmitted_) {
            finishedEmitted_ = true;
            state_ = PlaybackState::Stopped;
            forceRender_ = true;
            queue(PlaybackEvent::Finished);
        }
        return;
    }

    const double rawPos =
        anchorPos_ + (monotonicNow - anchorMonotonic_) * speed_ * fps;
    // Distance travelled in the direction of play (>= 0).
    double progress = speed_ > 0 ? (rawPos - static_cast<double>(segStart_))
                                 : (static_cast<double>(segEnd_) - rawPos);
    if (progress < 0.0) progress = 0.0;

    const double loopsD = std::floor(progress / L);
    if (loopsD >= finishAtLoop()) {
        currentFrame_ = speed_ > 0 ? segEnd_ : segStart_;
        currentPos_ = static_cast<double>(currentFrame_);
        if (!finishedEmitted_) {
            finishedEmitted_ = true;
            state_ = PlaybackState::Stopped;
            forceRender_ = true;
            queue(PlaybackEvent::Finished);
        }
        lastLoops_ = static_cast<long long>(loopsD);
        return;
    }

    const double phase = progress - loopsD * L;  // [0, L)
    currentPos_ = speed_ > 0 ? static_cast<double>(segStart_) + phase
                             : static_cast<double>(segEnd_) - phase;
    double fp = std::floor(currentPos_);
    if (fp < static_cast<double>(segStart_)) fp = static_cast<double>(segStart_);
    if (fp > static_cast<double>(segEnd_)) fp = static_cast<double>(segEnd_);
    currentFrame_ = static_cast<std::size_t>(fp);

    const long long loopsI = static_cast<long long>(loopsD);
    if (config_.loop && loopsI > lastLoops_) {
        queue(PlaybackEvent::Loop);
    }
    lastLoops_ = loopsI;
}

PlaybackController::Tick PlaybackController::advance(double monotonicNow) {
    if (state_ == PlaybackState::Playing) {
        if (needsAnchor_) {
            anchorMonotonic_ = monotonicNow;
            anchorPos_ = currentPos_;
            lastLoops_ = 0;
            needsAnchor_ = false;
        }
        advancePlaying(monotonicNow);
    }

    Tick t;
    t.frame = currentFrame_;
    if (!eventQueue_.empty()) {
        t.event = eventQueue_.front();
        eventQueue_.pop_front();
    }

    const bool frameChanged = !hasRendered_ || currentFrame_ != lastRenderedFrame_;
    if (forceRender_ || (state_ == PlaybackState::Playing && frameChanged)) {
        t.shouldRender = true;
        lastRenderedFrame_ = currentFrame_;
        hasRendered_ = true;
        forceRender_ = false;
    }
    return t;
}

PlaybackState PlaybackController::state() const { return state_; }

const AnimationMetadata& PlaybackController::metadata() const { return metadata_; }

}  // namespace rnrlottie
