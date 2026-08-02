// Chunk 7.1 — Metrics.h (MetricsCollector) + RenderCoordinator's
// setMetricsEnabled()/metricsSnapshot() wiring.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "Metrics.h"
#include "RenderCoordinator.h"
#include "test_harness.h"

using namespace rnrlottie;

namespace {

std::string fixture() { return tst::readText(tst::dataPath("fixtures/pixel-probe.json")); }

AnimationSource dataSource(const std::string& json) {
    AnimationSource s;
    s.kind = AnimationSource::Kind::Data;
    s.json = json;
    s.cacheKey = "metrics-probe";
    s.useModelCache = false;
    return s;
}

struct TestSink : FrameSink {
    std::mutex m;
    std::condition_variable cv;
    int published = 0, loaded = 0, errors = 0;

    void onFramePublished(std::uint64_t) override {
        std::lock_guard<std::mutex> lk(m);
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
};

const FrameBuffer::Limits kLimits{1u << 20};

}  // namespace

// --- MetricsCollector unit tests (no RenderCoordinator involved) -----------

TEST(metrics_percentiles_known_distribution) {
    MetricsCollector mc;
    mc.setEnabled(true);
    // 101 samples, values 1..101 in ARRIVAL order (not pre-sorted), so this
    // also exercises the internal sort. Nearest-rank over n=101:
    //   idx = floor(p * 100)  =>  p50 -> idx 50 -> value 51 (the median)
    //                             p95 -> idx 95 -> value 96
    //                             p99 -> idx 99 -> value 100
    std::vector<double> all;
    for (int v = 1; v <= 101; ++v) all.push_back(v);
    // Shuffle deterministically (no <random> dependency needed): reverse every
    // other pair, which is enough to prove the collector sorts correctly.
    for (std::size_t i = 0; i + 1 < all.size(); i += 2) std::swap(all[i], all[i + 1]);
    for (double v : all) mc.recordFramePublished(v);

    MetricsSnapshot s = mc.snapshot();
    CHECK(s.framesRendered == 101);
    CHECK(s.renderP50Ms == 51.0);
    CHECK(s.renderP95Ms == 96.0);
    CHECK(s.renderP99Ms == 100.0);
}

TEST(metrics_bounded_window_evicts_old_samples) {
    MetricsCollector mc;
    mc.setEnabled(true);
    // Push far more samples than kRenderWindowCapacity of huge outlier values
    // first, then fill the ENTIRE window with small values. If the window
    // were unbounded (a growing vector), the outliers would still dominate
    // the high percentiles; since it is a bounded ring buffer, they must be
    // fully evicted and the percentiles must reflect only the small values.
    for (std::size_t i = 0; i < MetricsCollector::kRenderWindowCapacity * 4; ++i) {
        mc.recordFramePublished(100000.0);  // outliers
    }
    for (std::size_t i = 0; i < MetricsCollector::kRenderWindowCapacity; ++i) {
        mc.recordFramePublished(static_cast<double>(i + 1));  // 1..capacity
    }
    MetricsSnapshot s = mc.snapshot();
    CHECK(s.framesRendered == MetricsCollector::kRenderWindowCapacity * 5);
    // Even p99 must be far below the outlier magnitude: proof the outliers
    // were evicted, not merely outnumbered.
    CHECK(s.renderP99Ms < 1000.0);
    CHECK(s.renderP99Ms >= static_cast<double>(MetricsCollector::kRenderWindowCapacity) * 0.9);
}

TEST(metrics_disabled_records_nothing) {
    MetricsCollector mc;  // disabled by default
    CHECK(!mc.enabled());
    mc.recordParseMs(12.0);
    mc.markSourceSet();
    mc.recordFramePublished(5.0);
    mc.recordFrameDropped();
    MetricsSnapshot s = mc.snapshot();
    CHECK(s.parseMs == 0.0);
    CHECK(s.firstFrameMs == 0.0);
    CHECK(s.framesRendered == 0);
    CHECK(s.framesDropped == 0);
    CHECK(s.renderP50Ms == 0.0 && s.renderP95Ms == 0.0 && s.renderP99Ms == 0.0);
}

TEST(metrics_first_frame_timing_arms_once_per_source) {
    MetricsCollector mc;
    mc.setEnabled(true);
    mc.markSourceSet();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    mc.recordFramePublished(1.0);
    const double first = mc.snapshot().firstFrameMs;
    CHECK(first >= 4.0);  // at least ~the sleep duration, monotonic clock

    // A second publish without a new markSourceSet() must NOT re-arm/change
    // firstFrameMs (it is latched until the next markSourceSet()).
    mc.recordFramePublished(1.0);
    CHECK(mc.snapshot().firstFrameMs == first);

    // A fresh markSourceSet() re-arms for the NEXT publish.
    mc.markSourceSet();
    mc.recordFramePublished(1.0);
    CHECK(mc.snapshot().firstFrameMs >= 0.0);
}

// --- RenderCoordinator wiring ------------------------------------------------

TEST(coord_metrics_disabled_by_default_stays_zero) {
    TestSink s;
    RenderCoordinator rc(s, kLimits);
    rc.setSurfaceSize(64, 64);
    rc.setSource(dataSource(fixture()));
    CHECK(s.waitFor([&] { return s.loaded >= 1; }));
    for (int i = 0; i < 20; ++i) rc.requestFrame(i % 31, rc.generation());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    MetricsSnapshot m = rc.metricsSnapshot();
    CHECK(m.parseMs == 0.0);
    CHECK(m.framesRendered == 0);
    CHECK(m.framesDropped == 0);
    // bufferAllocCount/peakBufferBytes come from FrameBuffer and are tracked
    // unconditionally, so they are non-zero even with metrics disabled.
    CHECK(m.bufferAllocCount >= 1);
    CHECK(m.peakBufferBytes > 0);
}

TEST(coord_metrics_enabled_end_to_end) {
    TestSink s;
    RenderCoordinator rc(s, kLimits);
    rc.setMetricsEnabled(true);
    rc.setSurfaceSize(64, 64);
    rc.setSource(dataSource(fixture()));
    CHECK(s.waitFor([&] { return s.loaded >= 1; }));
    rc.requestFrame(0, rc.generation());
    CHECK(s.waitFor([&] { return s.published >= 1; }));
    // Let a couple more frames land so the render-duration window is non-empty.
    for (int i = 1; i < 10; ++i) {
        rc.requestFrame(i % 31, rc.generation());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    MetricsSnapshot m = rc.metricsSnapshot();
    CHECK(m.parseMs >= 0.0);
    CHECK(m.firstFrameMs >= 0.0);
    CHECK(m.framesRendered >= 1);
    CHECK(m.renderP50Ms >= 0.0);
    CHECK(m.renderP99Ms >= m.renderP50Ms);
    CHECK(m.bufferAllocCount >= 1);
    CHECK(m.peakBufferBytes == static_cast<std::uint64_t>(64 * 64) * 4u * 2u);
}

// Dropped-frame counting under a request flood (mirrors coord_flood_coalesces
// but asserts on the metrics counters, not just the sink's published count).
TEST(coord_metrics_dropped_frames_under_flood) {
    TestSink s;
    RenderCoordinator rc(s, kLimits);
    rc.setMetricsEnabled(true);
    rc.setSurfaceSize(64, 64);
    rc.setSource(dataSource(fixture()));
    CHECK(s.waitFor([&] { return s.loaded >= 1; }));
    constexpr int kReq = 100000;
    for (int i = 0; i < kReq; ++i) rc.requestFrame(i % 31, rc.generation());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    MetricsSnapshot m = rc.metricsSnapshot();
    // Overwhelming majority of the flood must show up as dropped (coalesced),
    // and framesRendered+framesDropped must roughly track the request count
    // (allowing for the last still-in-flight/pending request).
    CHECK(m.framesDropped > static_cast<std::uint64_t>(kReq) / 2);
    CHECK(m.framesRendered >= 1);
    CHECK(m.framesRendered + m.framesDropped <= static_cast<std::uint64_t>(kReq));
    CHECK(m.framesRendered + m.framesDropped > static_cast<std::uint64_t>(kReq) - 4);
}

// Concurrent snapshot-while-rendering: metricsSnapshot() is read from a
// dedicated thread (standing in for the UI thread) while the render worker
// and several structural-change threads hammer the coordinator, same shape
// as coord_setters_concurrent_stress. Must be clean under TSan.
TEST(coord_metrics_concurrent_snapshot_stress) {
    TestSink s;
    RenderCoordinator rc(s, kLimits);
    rc.setMetricsEnabled(true);
    std::atomic<bool> run{true};
    const std::string json = fixture();

    std::thread tRender([&] {
        std::size_t f = 0;
        while (run.load()) rc.requestFrame((f++) % 31, rc.generation());
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
    std::atomic<std::uint64_t> snapshots{0};
    std::thread tSnapshot([&] {
        while (run.load()) {
            MetricsSnapshot m = rc.metricsSnapshot();
            (void)m;
            snapshots.fetch_add(1, std::memory_order_relaxed);
        }
    });
    // Flip metricsEnabled concurrently too, to exercise the enable/disable
    // gate racing with active collection.
    std::thread tToggle([&] {
        bool on = true;
        while (run.load()) {
            rc.setMetricsEnabled(on);
            on = !on;
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    run.store(false);
    tRender.join();
    tSize.join();
    tSource.join();
    tSnapshot.join();
    tToggle.join();
    rc.release();
    CHECK(snapshots.load() > 0);
}
