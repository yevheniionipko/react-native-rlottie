// Chunk 1.4 — the playback state machine + timing math.
//
// Pure logic: no threads, no rlottie, no pixels. Runs entirely on [main]. Given
// a monotonic clock each display tick, it decides which frame to render and
// which lifecycle events fired. Fully deterministic and unit-testable with a
// synthetic clock.
//
// Frame model (plan §5):
//   animationTime = anchorPos + (now - anchorMonotonic) * speed
//   frame         = segStart + floor(phase)              // phase within segment
// with segment range, repeatCount, loop rules and direction applied.
//
// Playback is ONE state (PlaybackState) with explicit transitions — never
// several unrelated booleans (plan §4).
#pragma once

#include <cstddef>
#include <deque>
#include <optional>

#include "AnimationMetadata.h"
#include "PlaybackState.h"
#include "PlayerError.h"

namespace rnrlottie {

struct PlaybackConfig {
    bool loop = false;
    int repeatCount = 0;   // when loop: 0 => infinite; N>0 => N extra plays
    double speed = 1.0;    // negative plays in reverse; 0 holds the current frame
    std::size_t startFrame = 0;
    std::size_t endFrame = 0;  // 0 => last frame; {0,0} => full range
    bool autoPlay = false;
};

enum class PlaybackEvent { None, Started, Paused, Loop, Finished };

class PlaybackController final {
public:
    struct Tick {
        std::size_t frame = 0;
        PlaybackEvent event = PlaybackEvent::None;
        bool shouldRender = false;
    };

    // Lifecycle from the loader.
    void onLoaded(const AnimationMetadata& metadata);  // Empty/Loading -> Ready (or Playing if autoPlay)
    void onLoadFailed(const PlayerError& error);        // -> Failed

    // Configuration & imperative commands (all [main]).
    void configure(const PlaybackConfig& config);
    void play(std::optional<std::size_t> startFrame, std::optional<std::size_t> endFrame);
    void pause();
    void resume();
    void stop();
    void reset();
    void seekToFrame(std::size_t frame);
    void seekToProgress(double progress);
    void setSpeed(double speed);

    // Called every display tick with a monotonic timestamp in SECONDS. Returns
    // the frame to render, one queued lifecycle event (if any), and whether a
    // render is needed this tick.
    Tick advance(double monotonicNow);

    PlaybackState state() const;
    const AnimationMetadata& metadata() const;

private:
    void resolveSegment();
    void advancePlaying(double monotonicNow);
    double finishAtLoop() const;  // wrap count at which playback finishes (may be +inf)
    bool hasAnimation() const;    // metadata loaded and not Failed/Empty/Loading
    void queue(PlaybackEvent event);

    AnimationMetadata metadata_;
    PlaybackConfig config_;
    PlaybackState state_ = PlaybackState::Empty;

    // Resolved inclusive segment bounds in frames.
    std::size_t segStart_ = 0;
    std::size_t segEnd_ = 0;

    double speed_ = 1.0;

    // Playback anchor (set lazily on the first tick after a command).
    double anchorMonotonic_ = 0.0;
    double anchorPos_ = 0.0;      // continuous frame position at the anchor
    bool needsAnchor_ = false;
    long long lastLoops_ = 0;     // completed wraps observed (for Loop events)

    double currentPos_ = 0.0;         // continuous frame position
    std::size_t currentFrame_ = 0;    // integer frame to display
    bool finishedEmitted_ = false;

    std::size_t lastRenderedFrame_ = 0;
    bool hasRendered_ = false;
    bool forceRender_ = false;

    std::deque<PlaybackEvent> eventQueue_;
};

}  // namespace rnrlottie
