// Chunk 1.7 — RenderCoordinator (the Phase 1 exit criterion, headless).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "RenderCoordinator.h"
#include "test_harness.h"

using namespace rnrlottie;

namespace {

std::string fixture() { return tst::readText(tst::dataPath("fixtures/pixel-probe.json")); }

AnimationSource dataSource(const std::string& json) {
    AnimationSource s;
    s.kind = AnimationSource::Kind::Data;
    s.json = json;
    s.cacheKey = "probe";
    s.useModelCache = false;
    return s;
}

struct TestSink : FrameSink {
    std::mutex m;
    std::condition_variable cv;
    int published = 0, loaded = 0, errors = 0;
    std::uint64_t lastGen = 0;
    bool monotonic = true;

    void onFramePublished(std::uint64_t g) override {
        std::lock_guard<std::mutex> lk(m);
        if (g < lastGen) monotonic = false;
        lastGen = g;
        ++published;
        cv.notify_all();
    }
    void onEvent(const PlayerEvent& e) override {
        std::lock_guard<std::mutex> lk(m);
        (e.type == PlayerEvent::Type::Loaded) ? ++loaded : ++errors;
        cv.notify_all();
    }
    bool waitFor(std::function<bool()> pred, int ms = 3000) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return pred(); });
    }
    int publishedSafe() {
        std::lock_guard<std::mutex> lk(m);
        return published;
    }
};

const FrameBuffer::Limits kLimits{1u << 20};

}  // namespace

TEST(coord_load_render_publish_golden) {
    TestSink s;
    RenderCoordinator rc(s, kLimits);
    rc.setSurfaceSize(64, 64);
    rc.setSource(dataSource(fixture()));
    CHECK(s.waitFor([&] { return s.loaded >= 1; }));
    rc.requestFrame(0, rc.generation());
    CHECK(s.waitFor([&] { return s.published >= 1; }));
    auto fb = rc.frameBuffer().readFront();
    CHECK(fb.pixels != nullptr);
    CHECK(fb.width == 64 && fb.height == 64);
    CHECK(fb.pixels[10 * 64 + 10] == 0xFFFF0000u);
    CHECK(fb.pixels[54 * 64 + 54] == 0x7F7F0000u);
    CHECK(s.errors == 0);
}

TEST(coord_request_before_size_no_publish) {
    TestSink s;
    RenderCoordinator rc(s, kLimits);
    rc.setSource(dataSource(fixture()));
    CHECK(s.waitFor([&] { return s.loaded >= 1; }));
    rc.requestFrame(0, rc.generation());
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(s.publishedSafe() == 0);
}

TEST(coord_no_callback_after_release) {
    TestSink s;
    RenderCoordinator rc(s, kLimits);
    rc.setSurfaceSize(64, 64);
    rc.setSource(dataSource(fixture()));
    for (int k = 0; k < 200; ++k) rc.requestFrame(k % 31, rc.generation());
    rc.release();
    const int snap = s.publishedSafe();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(s.publishedSafe() == snap);
}

TEST(coord_flood_coalesces) {
    TestSink s;
    RenderCoordinator rc(s, kLimits);
    rc.setSurfaceSize(64, 64);
    rc.setSource(dataSource(fixture()));
    CHECK(s.waitFor([&] { return s.loaded >= 1; }));
    constexpr int kReq = 100000;
    for (int i = 0; i < kReq; ++i) rc.requestFrame(i % 31, rc.generation());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const int pub = s.publishedSafe();
    CHECK(pub >= 1 && pub < kReq / 10);
    CHECK(s.monotonic);
}

TEST(coord_teardown_during_render_stress) {
    for (int iter = 0; iter < 120; ++iter) {
        TestSink s;
        RenderCoordinator rc(s, kLimits);
        rc.setSurfaceSize(64, 64);
        rc.setSource(dataSource(fixture()));
        for (int k = 0; k < 40; ++k) rc.requestFrame(k % 31, rc.generation());
    }  // dtor -> release(): cancel, destroy core on worker, join
}

// Chunk 6.1 — setOpacity/setStrokeWidth must be routed through the SAME
// control-queue/worker mechanism as setColor (never touching core_ from the
// caller thread). Structural proof: RenderCoordinator::setOpacity/
// setStrokeWidth enqueue onto controlQueue_ exactly like setColor (see
// RenderCoordinator.cpp) and are only ever invoked from workerLoop(). This
// test exercises that path end to end (enqueue -> worker applies -> a render
// still succeeds) and, combined with coord_setters_concurrent_stress below,
// gives TSan a real chance to catch a routing mistake.
TEST(coord_opacity_strokewidth_route_through_worker) {
    TestSink s;
    RenderCoordinator rc(s, kLimits);
    rc.setSurfaceSize(64, 64);
    rc.setSource(dataSource(fixture()));
    CHECK(s.waitFor([&] { return s.loaded >= 1; }));
    rc.setOpacity("**", 0.5f);
    rc.setStrokeWidth("**", 3.0f);
    rc.requestFrame(0, rc.generation());
    CHECK(s.waitFor([&] { return s.published >= 1; }));
    CHECK(s.errors == 0);
}

// Chunk 6.2 — hammer setColor/setOpacity/setStrokeWidth from multiple threads
// concurrently with rendering and structural changes. Must be clean under
// TSan: every setter call touches core_ only via the worker (RenderCoordinator
// invariant, CLAUDE.md), so no data race should be observable regardless of
// timing.
TEST(coord_setters_concurrent_stress) {
    TestSink s;
    RenderCoordinator rc(s, kLimits);
    std::atomic<bool> run{true};
    const std::string json = fixture();

    std::thread tRender([&] {
        std::size_t f = 0;
        while (run.load()) rc.requestFrame((f++) % 31, rc.generation());
    });
    std::thread tColor([&] {
        float g = 0.0f;
        while (run.load()) {
            rc.setColor("**", g, 1.0f - g, 0.5f);
            g = g > 1.0f ? 0.0f : g + 0.05f;
        }
    });
    std::thread tOpacity([&] {
        float o = 0.0f;
        while (run.load()) {
            rc.setOpacity("**", o);
            o = o > 1.2f ? -0.2f : o + 0.05f;  // sweeps past the clamp bounds
        }
    });
    std::thread tStroke([&] {
        float w = -1.0f;
        while (run.load()) {
            rc.setStrokeWidth("**", w);
            w = w > 5.0f ? -1.0f : w + 0.2f;  // sweeps past the clamp bound
        }
    });
    std::thread tSize([&] {
        std::size_t d = 0;
        while (run.load()) {
            rc.setSurfaceSize(32 + (d % 64), 32 + (d % 64));
            ++d;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    std::thread tSource([&] {
        while (run.load()) {
            rc.setSource(dataSource(json));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    run.store(false);
    tRender.join();
    tColor.join();
    tOpacity.join();
    tStroke.join();
    tSize.join();
    tSource.join();
    rc.release();
    CHECK(s.monotonic);
}

TEST(coord_concurrent_api_monotonic) {
    TestSink s;
    RenderCoordinator rc(s, kLimits);
    std::atomic<bool> run{true};
    const std::string json = fixture();

    std::thread t1([&] {
        std::size_t f = 0;
        while (run.load()) rc.requestFrame((f++) % 31, rc.generation());
    });
    std::thread t2([&] {
        std::size_t d = 0;
        while (run.load()) {
            rc.setSurfaceSize(32 + (d % 64), 32 + (d % 64));
            ++d;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    std::thread t3([&] {
        while (run.load()) {
            rc.setSource(dataSource(json));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    run.store(false);
    t1.join();
    t2.join();
    t3.join();
    rc.release();
    CHECK(s.monotonic);
}
