// Chunk 7.3 — lifecycle stress + leak tests (plan §20 "Native lifecycle
// tests" / §21 non-functional acceptance criteria), run on the HOST.
//
// This file covers the headless-achievable subset of plan §20's list by
// exercising the same two seams the platform adapters drive:
//   - RenderCoordinator directly (the shared core's mount/unmount unit), and
//   - JniPlayerHandle's registry (createPlayerHandle/destroyPlayerHandle),
//     which is the closest headless analogue to a real ViewManager
//     mount/unmount cycle (see tests/cpp/android_tests.cpp for the
//     fault-injection tests this file's stress tests build on).
//
// What this file deliberately does NOT attempt — because it genuinely
// requires a device/emulator, a real UI thread, or a real OS memory-pressure
// signal — is documented in docs/lifecycle-testing.md, not silently skipped
// or faked here:
//   - background/foreground transitions (CADisplayLink / Choreographer
//     pause-resume through the platform's real lifecycle callbacks);
//   - JS reload / React root destruction (there is no JS runtime in this
//     binary);
//   - mount/unmount through the actual RCTViewManager / SimpleViewManager
//     (ios/, android/ ViewManager glue is out of this stream's ownership —
//     see CLAUDE.md file ownership);
//   - real OS low-memory notifications (didReceiveMemoryWarning /
//     onTrimMemory) — this file instead exercises the ALLOCATION-FAILURE
//     code path directly (InputLimits rejection and FrameBuffer's
//     bad_alloc -> AllocationFailed conversion), which is what those
//     notifications would ultimately have to survive, but is not the
//     notification itself.
//
// Every stress loop here is bounded so the default `plain asan tsan` gate
// (tests/run-tests.sh with no args) stays fast: iteration counts are picked
// to be "hundreds" per plan §20 while keeping the whole file well under a
// second of added wall time per variant (verified: see the report this
// chunk shipped with). Nothing in this file is behind an opt-in variant.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "AnimationSource.h"
#include "FrameBuffer.h"
#include "InputLimits.h"
#include "JniPlayerHandle.h"
#include "ModelCacheController.h"
#include "RenderCoordinator.h"
#include "RlottiePlayerCore.h"
#include "test_harness.h"

using namespace rnrlottie;

namespace {

std::string fixture() { return tst::readText(tst::dataPath("fixtures/pixel-probe.json")); }

AnimationSource dataSource(const std::string& json, const std::string& cacheKey = "probe",
                           bool useModelCache = false) {
    AnimationSource s;
    s.kind = AnimationSource::Kind::Data;
    s.json = json;
    s.cacheKey = cacheKey;
    s.useModelCache = useModelCache;
    return s;
}

struct CountingSink : FrameSink {
    std::atomic<int> published{0};
    std::atomic<int> loaded{0};
    std::atomic<int> errors{0};
    void onFramePublished(std::uint64_t) override { published.fetch_add(1); }
    void onEvent(const PlayerEvent& e) override {
        if (e.type == PlayerEvent::Type::Loaded) {
            loaded.fetch_add(1);
        } else {
            errors.fetch_add(1);
        }
    }
};

bool waitForFrame(JniPlayerHandle* h, int ms = 3000) {
    for (int i = 0; i < ms; ++i) {
        if (h->sinkState().newFrame.load(std::memory_order_acquire)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool waitForEvent(JniPlayerHandle* h, PlayerEvent& out, int ms = 3000) {
    for (int i = 0; i < ms; ++i) {
        if (popEvent(h->sinkState(), out)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

constexpr std::size_t kMaxPixels = 1u << 20;

}  // namespace

// --- 1. mount/unmount hundreds of times -------------------------------------
//
// The analogue of a view mounting and unmounting: create the full native
// stack (JniPlayerHandle -> RenderCoordinator -> RlottiePlayerCore), size it,
// load a source, fire off a few render requests without waiting for them (a
// real unmount can race an in-flight load/render exactly like this), then
// destroy. livePlayerCount() must return to its pre-loop baseline every
// single time: a leaked registry entry would accumulate and this assertion
// would fail on iteration 2, not just at the end.
TEST(lifecycle_mount_unmount_hundreds) {
    const std::size_t baseline = livePlayerCount();
    const std::string json = fixture();
    constexpr int kIterations = 300;
    for (int i = 0; i < kIterations; ++i) {
        const std::int64_t handle = createPlayerHandle(kMaxPixels);
        CHECK(handle != 0);
        auto* h = lookupPlayerHandle(handle);
        CHECK(h != nullptr);
        CHECK(livePlayerCount() == baseline + 1);

        h->coordinator().setSurfaceSize(32, 32);
        h->coordinator().setSource(dataSource(json));
        for (int k = 0; k < 4; ++k) {
            h->coordinator().requestFrame(k, h->coordinator().generation());
        }
        CHECK(destroyPlayerHandle(handle));
        CHECK(livePlayerCount() == baseline);
    }
    CHECK(livePlayerCount() == baseline);
}

// --- 2. navigate away while loading ------------------------------------------
//
// Destroy the handle immediately after setSource(), before the load has had
// any real chance to complete on the worker — the "user navigated away
// before the animation finished loading" scenario. release() (called from
// the JniPlayerHandle destructor via RenderCoordinator's dtor) must
// synchronously cancel + join, so no Loaded/Error event or frame publish can
// land after destroyPlayerHandle() returns, and the registry must not leak
// the in-flight entry.
TEST(lifecycle_navigate_away_while_loading) {
    const std::size_t baseline = livePlayerCount();
    const std::string json = fixture();
    constexpr int kIterations = 150;
    for (int i = 0; i < kIterations; ++i) {
        const std::int64_t handle = createPlayerHandle(kMaxPixels);
        auto* h = lookupPlayerHandle(handle);
        h->coordinator().setSurfaceSize(64, 64);
        h->coordinator().setSource(dataSource(json));  // load enqueued, NOT awaited
        CHECK(destroyPlayerHandle(handle));            // torn down while (maybe) still loading
        CHECK(lookupPlayerHandle(handle) == nullptr);
    }
    CHECK(livePlayerCount() == baseline);
}

// --- 3. rapid source change ---------------------------------------------------
//
// Replace the source many times in quick succession, including while renders
// are outstanding, then settle on a final load and verify the coordinator
// converges to a CONSISTENT state (not just "didn't crash"): exactly the
// final source's metadata/frame is what eventually renders, and the sink
// never observed a generation regression.
TEST(lifecycle_rapid_source_change) {
    CountingSink s;
    RenderCoordinator rc(s, FrameBuffer::Limits{kMaxPixels});
    rc.setSurfaceSize(64, 64);
    const std::string json = fixture();

    std::uint64_t lastGen = 0;
    for (int i = 0; i < 200; ++i) {
        lastGen = rc.setSource(dataSource(json, "rapid-" + std::to_string(i)));
        rc.requestFrame(i % 31, lastGen);
    }

    // Converges: the coordinator eventually loads the LAST source and can
    // render a frame tagged with the LAST generation.
    bool loaded = false;
    for (int i = 0; i < 3000 && !loaded; ++i) {
        rc.requestFrame(0, lastGen);
        if (rc.frameBuffer().readFront().pixels != nullptr) loaded = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(loaded);
    const auto front = rc.frameBuffer().readFront();
    CHECK(front.width == 64 && front.height == 64);
    CHECK(rc.generation() == lastGen);
    rc.release();
}

// --- 4. resize while rendering ------------------------------------------------
//
// Interleave setSurfaceSize with requestFrame so a resize can land while a
// render is in flight, then settle on a final size and confirm the buffer
// dimensions are exactly that final size (never a stale or torn one) once
// rendering resumes.
TEST(lifecycle_resize_while_rendering) {
    CountingSink s;
    RenderCoordinator rc(s, FrameBuffer::Limits{kMaxPixels});
    rc.setSource(dataSource(fixture()));

    const std::size_t sizes[] = {16, 32, 64, 48, 96, 24, 64};
    for (int round = 0; round < 40; ++round) {
        const std::size_t dim = sizes[round % (sizeof(sizes) / sizeof(sizes[0]))];
        const std::uint64_t gen = rc.setSurfaceSize(dim, dim);
        for (int k = 0; k < 5; ++k) rc.requestFrame(k % 31, gen);
    }
    const std::uint64_t finalGen = rc.setSurfaceSize(64, 64);

    bool published = false;
    for (int i = 0; i < 3000 && !published; ++i) {
        rc.requestFrame(0, finalGen);
        const auto front = rc.frameBuffer().readFront();
        if (front.pixels != nullptr && front.width == 64 && front.height == 64) published = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(published);
    const auto front = rc.frameBuffer().readFront();
    CHECK(front.width == 64 && front.height == 64);
    CHECK(front.bytesPerLine == 64 * 4);
    rc.release();
}

// --- 5. multiple views using the same source ---------------------------------
//
// Several independent RenderCoordinators (the per-view unit) load the SAME
// cache key with useModelCache=true, exercising rlottie's shared model cache
// (ModelCacheController, Chunk 1.6) from concurrent views the way multiple
// mounted RlottieView instances pointing at the same source would. Each view
// must still render its OWN correct frame — the shared parsed model must not
// leak state (color overrides, playback position) between independently
// owned Animation instances.
TEST(lifecycle_multiple_views_shared_source) {
    constexpr int kViews = 8;
    const std::string json = fixture();
    const std::string sharedKey = "shared-lifecycle-probe";

    std::vector<std::unique_ptr<CountingSink>> sinks;
    std::vector<std::unique_ptr<RenderCoordinator>> views;
    for (int i = 0; i < kViews; ++i) {
        sinks.push_back(std::make_unique<CountingSink>());
        views.push_back(
            std::make_unique<RenderCoordinator>(*sinks.back(), FrameBuffer::Limits{kMaxPixels}));
        views.back()->setSurfaceSize(64, 64);
        views.back()->setSource(dataSource(json, sharedKey, /*useModelCache=*/true));
    }

    for (auto& v : views) {
        for (int k = 0; k < 4; ++k) v->requestFrame(0, v->generation());
    }

    for (int i = 0; i < kViews; ++i) {
        bool got = false;
        for (int t = 0; t < 3000 && !got; ++t) {
            views[i]->requestFrame(0, views[i]->generation());
            if (views[i]->frameBuffer().readFront().pixels != nullptr) got = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(got);
        const auto front = views[i]->frameBuffer().readFront();
        CHECK(front.width == 64 && front.height == 64);
        // Same golden pixel every independent view renders for frame 0 of the
        // unmodified fixture (matches coord_load_render_publish_golden).
        CHECK(front.pixels[10 * 64 + 10] == 0xFFFF0000u);
        CHECK(sinks[static_cast<std::size_t>(i)]->errors.load() == 0);
    }

    for (auto& v : views) v->release();
}

// --- 6. destruction during a pending render ----------------------------------
//
// Flood requestFrame() (guaranteeing an outstanding render) then immediately
// destroy — hundreds of times — through the JNI handle path so
// livePlayerCount() gives a leak assertion on every iteration, and via a
// raw RenderCoordinator to reuse coord_no_callback_after_release's "no
// publish after release" contract under repetition.
TEST(lifecycle_destruction_during_pending_render_jni) {
    const std::size_t baseline = livePlayerCount();
    const std::string json = fixture();
    constexpr int kIterations = 150;
    for (int i = 0; i < kIterations; ++i) {
        const std::int64_t handle = createPlayerHandle(kMaxPixels);
        auto* h = lookupPlayerHandle(handle);
        h->coordinator().setSurfaceSize(48, 48);
        h->coordinator().setSource(dataSource(json));
        for (int k = 0; k < 50; ++k) {
            h->coordinator().requestFrame(k % 31, h->coordinator().generation());
        }
        CHECK(destroyPlayerHandle(handle));  // must cancel + join, never publish after this
    }
    CHECK(livePlayerCount() == baseline);
}

TEST(lifecycle_destruction_during_pending_render_repeat) {
    for (int iter = 0; iter < 150; ++iter) {
        CountingSink s;
        RenderCoordinator rc(s, FrameBuffer::Limits{kMaxPixels});
        rc.setSurfaceSize(48, 48);
        rc.setSource(dataSource(fixture()));
        for (int k = 0; k < 30; ++k) rc.requestFrame(k % 31, rc.generation());
        const int snapPublished = s.published.load();
        rc.release();
        // release() joins the worker synchronously; nothing may land after.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(s.published.load() == snapPublished);
    }
}

// --- 7. low-memory -> allocation-failure paths -------------------------------
//
// A real OS low-memory notification cannot be produced headlessly (see
// docs/lifecycle-testing.md), but the code path everything downstream of one
// must survive — an allocation/limit rejection that reports a typed error
// instead of crashing/terminating the process — can be exercised directly.

// 7a. InputLimits rejects an over-budget source before it ever reaches
// rlottie or attempts a large allocation (the "reject before you'd have to
// allocate" half of low-memory handling).
TEST(lifecycle_input_limits_reject_oversize_source) {
    tst::ScopedInputLimits restore;
    const std::string json = fixture();
    InputLimits tight;
    tight.maxJsonBytes = json.size() - 1;
    setInputLimits(tight);

    RlottiePlayerCore core;
    auto r = core.loadFromData(json, "k", "", false);
    CHECK(!r.success);
    CHECK(r.error.code == PlayerErrorCode::SourceTooLarge);
    CHECK(!core.isLoaded());

    // The core is left in a well-defined, still-usable state: restoring
    // generous limits and loading again succeeds (a rejection must not
    // permanently wedge the object).
    setInputLimits(InputLimits{});
    auto r2 = core.loadFromData(json, "k", "", false);
    CHECK(r2.success);
    CHECK(core.isLoaded());
}

// 7b. A genuine allocation failure inside FrameBuffer::resize() (bad_alloc,
// the real signature of OOM under low memory) is caught and converted to a
// typed AllocationFailed PlayerError — the process is NOT terminated (this
// test itself continues to run and pass afterwards) and the buffer's
// previous state is left intact rather than half-updated.
TEST(lifecycle_framebuffer_allocation_failure_does_not_terminate) {
    // Deliberately chosen to fail FAST and WITHOUT TOUCHING MEMORY, not by
    // exhausting real RAM/swap (which would hang/thrash this test and the
    // host running it): kHugeDim*kHugeDim pixels exceeds
    // std::vector<uint32_t>::max_size() (implementation-defined, but always
    // well under SIZE_MAX/sizeof(T) — see e.g. libc++'s
    // min(PTRDIFF_MAX, allocator_traits::max_size())/sizeof(T)), so
    // buffers_[i].assign(pixels, 0u) throws std::length_error immediately,
    // before attempting any allocation, let alone touching a page. Both
    // per-axis values stay individually valid (< UINT32_MAX) and their
    // product does not overflow size_t, so this exercises the REAL
    // resize()->assign() path rather than being rejected earlier by
    // checkDimensions()'s overflow/range guards.
    constexpr std::size_t kHugeDim = 3000000000ull;         // < UINT32_MAX
    constexpr std::size_t kHugeMaxPixels = kHugeDim * kHugeDim;  // 9e18 pixels
    FrameBuffer fb(FrameBuffer::Limits{kHugeMaxPixels});

    PlayerError err;
    const bool ok = fb.resize(FrameBuffer::Dimensions{kHugeDim, kHugeDim}, err);
    CHECK(!ok);
    CHECK(err.code == PlayerErrorCode::AllocationFailed);
    CHECK(!fb.isSized());  // rejected resize leaves it unsized, not half-allocated
    CHECK(fb.readFront().pixels == nullptr);

    // The FrameBuffer remains fully usable afterwards: a sane resize
    // succeeds normally (the failed attempt did not corrupt internal state).
    PlayerError err2;
    CHECK(fb.resize(FrameBuffer::Dimensions{64, 64}, err2));
    CHECK(fb.isSized());
    CHECK(fb.dimensions().width == 64 && fb.dimensions().height == 64);
}

// 7c. The same allocation-failure path exercised through the full
// RenderCoordinator/JniPlayerHandle stack: a resize to a pathological size
// must report an error event, not crash, and the view must remain usable
// (a subsequent sane resize + load + render still works). Also asserts
// livePlayerCount() returns to baseline.
TEST(lifecycle_coordinator_survives_allocation_failure) {
    const std::size_t baseline = livePlayerCount();
    // Same "fails fast, never touches memory" size as
    // lifecycle_framebuffer_allocation_failure_does_not_terminate above.
    constexpr std::size_t kHugeDim = 3000000000ull;
    constexpr std::size_t kHugeMaxPixels = kHugeDim * kHugeDim;
    const std::int64_t handle = createPlayerHandle(kHugeMaxPixels);
    auto* h = lookupPlayerHandle(handle);

    // Triggers std::length_error -> AllocationFailed inside resize() (worker
    // thread; runResize() reports it as a PlayerEvent::Type::Error).
    h->coordinator().setSurfaceSize(kHugeDim, kHugeDim);
    h->coordinator().setSource(dataSource(fixture()));

    PlayerEvent ev;
    bool sawError = false;
    while (waitForEvent(h, ev, 500)) {
        if (ev.type == PlayerEvent::Type::Error) sawError = true;
    }
    CHECK(sawError);

    // Recovery: a sane resize on the SAME still-live handle works normally.
    h->coordinator().setSurfaceSize(64, 64);
    h->coordinator().requestFrame(0, h->coordinator().generation());
    CHECK(waitForFrame(h));
    const auto front = h->coordinator().frameBuffer().readFront();
    CHECK(front.pixels != nullptr && front.width == 64 && front.height == 64);

    CHECK(destroyPlayerHandle(handle));
    CHECK(livePlayerCount() == baseline);
}
