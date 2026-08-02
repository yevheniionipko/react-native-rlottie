// Chunk 1.7 — PlaybackController state machine + timing (synthetic clock).
#include "PlaybackController.h"
#include "test_harness.h"

using namespace rnrlottie;

namespace {

AnimationMetadata meta(double fps, std::size_t total) {
    AnimationMetadata m;
    m.width = 64;
    m.height = 64;
    m.frameRate = fps;
    m.totalFrames = total;
    m.duration = total / fps;
    return m;
}

struct Drive {
    int started = 0, paused = 0, loops = 0, finished = 0;
    std::size_t lastFrame = 0;
};

Drive drive(PlaybackController& pc, double t0, double t1, int steps) {
    Drive d;
    for (int i = 0; i <= steps; ++i) {
        const double t = t0 + (t1 - t0) * (static_cast<double>(i) / steps);
        auto tick = pc.advance(t);
        switch (tick.event) {
            case PlaybackEvent::Started: ++d.started; break;
            case PlaybackEvent::Paused: ++d.paused; break;
            case PlaybackEvent::Loop: ++d.loops; break;
            case PlaybackEvent::Finished: ++d.finished; break;
            case PlaybackEvent::None: break;
        }
        d.lastFrame = tick.frame;
    }
    return d;
}

}  // namespace

TEST(playback_ready_renders_first_frame) {
    PlaybackController pc;
    pc.configure(PlaybackConfig{});
    pc.onLoaded(meta(10, 11));
    CHECK(pc.state() == PlaybackState::Ready);
    auto t0 = pc.advance(0.0);
    CHECK(t0.shouldRender && t0.frame == 0);
    CHECK(!pc.advance(0.1).shouldRender);
}

TEST(playback_once_finishes_exactly_once) {
    PlaybackController pc;
    pc.onLoaded(meta(10, 11));
    pc.play(std::nullopt, std::nullopt);
    auto d = drive(pc, 0.0, 1.5, 300);
    CHECK(d.started == 1);
    CHECK(d.finished == 1);
    CHECK(d.loops == 0);
    CHECK(d.lastFrame == 10);
    CHECK(pc.state() == PlaybackState::Stopped);
}

TEST(playback_infinite_loop) {
    PlaybackController pc;
    pc.configure(PlaybackConfig{true, 0});
    pc.onLoaded(meta(10, 11));
    pc.play(std::nullopt, std::nullopt);
    auto d = drive(pc, 0.0, 3.5, 700);
    CHECK(d.finished == 0);
    CHECK(d.loops == 3);
    CHECK(pc.state() == PlaybackState::Playing);
}

TEST(playback_finite_repeat) {
    PlaybackController pc;
    pc.configure(PlaybackConfig{true, 2});
    pc.onLoaded(meta(10, 11));
    pc.play(std::nullopt, std::nullopt);
    auto d = drive(pc, 0.0, 3.5, 700);
    CHECK(d.loops == 2);
    CHECK(d.finished == 1);
    CHECK(d.lastFrame == 10);
}

TEST(playback_segment_range) {
    PlaybackController pc;
    PlaybackConfig c;
    c.startFrame = 2;
    c.endFrame = 5;
    pc.configure(c);
    pc.onLoaded(meta(10, 11));
    CHECK(pc.advance(0.0).frame == 2);
    pc.play(std::nullopt, std::nullopt);
    std::size_t minF = 99, maxF = 0;
    for (int i = 0; i <= 100; ++i) {
        auto tk = pc.advance(i / 100.0 * 0.5);
        if (tk.frame < minF) minF = tk.frame;
        if (tk.frame > maxF) maxF = tk.frame;
    }
    CHECK(minF >= 2 && maxF == 5);
}

TEST(playback_reverse) {
    PlaybackController pc;
    pc.configure(PlaybackConfig{false, 0, -1.0});
    pc.onLoaded(meta(10, 11));
    pc.play(std::nullopt, std::nullopt);
    CHECK(pc.advance(0.0).frame == 10);
    auto d = drive(pc, 0.01, 1.2, 240);
    CHECK(d.finished == 1);
    CHECK(d.lastFrame == 0);
}

TEST(playback_zero_speed_holds) {
    PlaybackController pc;
    pc.configure(PlaybackConfig{false, 0, 0.0});
    pc.onLoaded(meta(10, 11));
    pc.play(std::nullopt, std::nullopt);
    auto d = drive(pc, 0.0, 5.0, 200);
    CHECK(d.finished == 0);
    CHECK(d.lastFrame == 0);
    CHECK(pc.state() == PlaybackState::Playing);
}

TEST(playback_seek_clamping) {
    PlaybackController pc;
    pc.onLoaded(meta(10, 11));
    pc.seekToFrame(100);
    CHECK(pc.advance(0.0).frame == 10);
    pc.seekToProgress(0.5);
    CHECK(pc.advance(0.1).frame == 5);
    pc.seekToProgress(-3.0);
    CHECK(pc.advance(0.2).frame == 0);
}

TEST(playback_pause_resume) {
    PlaybackController pc;
    pc.onLoaded(meta(10, 11));
    pc.play(std::nullopt, std::nullopt);
    (void)pc.advance(0.0);
    CHECK(pc.advance(0.25).frame == 2);
    pc.pause();
    CHECK(pc.advance(0.30).event == PlaybackEvent::Paused);
    CHECK(pc.state() == PlaybackState::Paused);
    auto frozen = pc.advance(0.60);
    CHECK(frozen.frame == 2 && !frozen.shouldRender);
    pc.resume();
    CHECK(pc.state() == PlaybackState::Playing);
    CHECK(pc.advance(0.60).frame == 2);
}

TEST(playback_load_failure_inert) {
    PlaybackController pc;
    pc.onLoadFailed(PlayerError{PlayerErrorCode::ParseFailed, "x", 0});
    CHECK(pc.state() == PlaybackState::Failed);
    pc.play(std::nullopt, std::nullopt);
    CHECK(pc.state() == PlaybackState::Failed);
}
