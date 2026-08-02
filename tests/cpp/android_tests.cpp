// Chunk 3.1 — Android JNI-layer tests (handle safety, pixel conversion, event
// wire format), run on the HOST.
//
// The three files under test (JniPlayerHandle, AndroidFrameSink,
// AndroidPixelConvert/AndroidEventEncoding) are deliberately JNI-free, so the
// logic that RlottieJni.cpp calls is exercised here without an emulator.
// RlottieJni.cpp itself is a thin translation layer and is compile-verified
// against the NDK by scripts/check-android-build.sh.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "AndroidEventEncoding.h"
#include "AndroidFrameSink.h"
#include "AndroidPixelConvert.h"
#include "JniPlayerHandle.h"
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

constexpr std::size_t kMaxPixels = 1u << 20;

// Drive the handle the way RlottieView will: request a frame, then poll until
// the worker publishes one.
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

}  // namespace

// --- Handle safety (plan §10/§22 — the UAF guard) ---------------------------

TEST(jni_handle_roundtrip) {
    const std::int64_t handle = createPlayerHandle(kMaxPixels);
    CHECK(handle != 0);
    auto* h = lookupPlayerHandle(handle);
    CHECK(h != nullptr);
    CHECK(h->valid());
    CHECK(lookupPlayerHandle(handle) == h);  // stable
    CHECK(destroyPlayerHandle(handle));
}

TEST(jni_zero_handle_fails_safe) {
    CHECK(lookupPlayerHandle(0) == nullptr);
    CHECK(!destroyPlayerHandle(0));
}

TEST(jni_double_destroy_is_idempotent) {
    const std::int64_t handle = createPlayerHandle(kMaxPixels);
    CHECK(destroyPlayerHandle(handle));
    // The player is gone and its id is never reused, so these miss the registry
    // instead of touching freed memory. Under ASan a regression to a
    // pointer-plus-sentinel scheme shows up here as a heap-use-after-free.
    CHECK(!destroyPlayerHandle(handle));
    CHECK(!destroyPlayerHandle(handle));
    CHECK(lookupPlayerHandle(handle) == nullptr);
}

// Fault injection: ids the library never minted — a Kotlin bug, a recycled
// field, or a hostile value — must miss the registry, not be dereferenced.
TEST(jni_forged_handles_rejected) {
    const std::int64_t live = createPlayerHandle(kMaxPixels);
    CHECK(live != 0);
    for (std::int64_t bad : {std::int64_t{-1}, std::int64_t{-1000000},
                             std::int64_t{0x7FFFFFFFFFFFFFFF}, live + 1, live + 999,
                             std::int64_t{0xDEADBEEF}}) {
        CHECK(lookupPlayerHandle(bad) == nullptr);
        CHECK(!destroyPlayerHandle(bad));
    }
    CHECK(lookupPlayerHandle(live) != nullptr);  // the real one is untouched
    CHECK(destroyPlayerHandle(live));
}

// Every handle-taking JNI entry point guards with `if (h == nullptr) return;`
// on a destroyed handle — nothing runs.
TEST(jni_operations_on_invalid_handle_no_op) {
    const std::int64_t handle = createPlayerHandle(kMaxPixels);
    CHECK(destroyPlayerHandle(handle));

    for (std::int64_t bad : {std::int64_t{0}, handle}) {
        auto* h = lookupPlayerHandle(bad);
        CHECK(h == nullptr);
        if (h != nullptr) {
            h->coordinator().reset();  // never reached
        }
    }
}

TEST(jni_registry_releases_every_player) {
    const std::size_t before = livePlayerCount();
    std::vector<std::int64_t> handles;
    for (int i = 0; i < 8; ++i) {
        handles.push_back(createPlayerHandle(kMaxPixels));
    }
    CHECK(livePlayerCount() == before + handles.size());
    for (std::int64_t handle : handles) {
        CHECK(destroyPlayerHandle(handle));
    }
    CHECK(livePlayerCount() == before);
}

// Ids are minted from a monotonic counter, so a destroyed view's stale id can
// never be resolved to a NEW player that reused its slot.
TEST(jni_ids_are_never_reused) {
    const std::int64_t first = createPlayerHandle(kMaxPixels);
    CHECK(destroyPlayerHandle(first));
    const std::int64_t second = createPlayerHandle(kMaxPixels);
    CHECK(second != first);
    CHECK(lookupPlayerHandle(first) == nullptr);
    CHECK(destroyPlayerHandle(second));
}

TEST(jni_destroy_after_work_no_callback) {
    const std::int64_t handle = createPlayerHandle(kMaxPixels);
    auto* h = lookupPlayerHandle(handle);
    CHECK(h != nullptr);
    h->coordinator().setSurfaceSize(64, 64);
    h->coordinator().setSource(dataSource(fixture()));
    for (int k = 0; k < 200; ++k) {
        h->coordinator().requestFrame(k % 31, h->coordinator().generation());
    }
    const std::uint64_t genBefore = h->sinkState().lastPublishedGeneration.load();
    CHECK(destroyPlayerHandle(handle));
    // Nothing may run after destroy: the coordinator joined its worker inside
    // destroy, so no sink write can land in freed memory. Under ASan/TSan a
    // regression here shows up as a use-after-free / data race rather than a
    // CHECK failure; the assertion documents the intent.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(lookupPlayerHandle(handle) == nullptr);
    (void)genBefore;
}

TEST(jni_create_destroy_stress) {
    for (int i = 0; i < 100; ++i) {
        const std::int64_t handle = createPlayerHandle(kMaxPixels);
        CHECK(handle != 0);
        auto* h = lookupPlayerHandle(handle);
        h->coordinator().setSurfaceSize(32, 32);
        h->coordinator().setSource(dataSource(fixture()));
        for (int k = 0; k < 8; ++k) {
            h->coordinator().requestFrame(k, h->coordinator().generation());
        }
        CHECK(destroyPlayerHandle(handle));
        CHECK(!destroyPlayerHandle(handle));
    }
}

// --- Poll-based sink --------------------------------------------------------

TEST(jni_sink_publishes_and_queues) {
    const std::int64_t handle = createPlayerHandle(kMaxPixels);
    auto* h = lookupPlayerHandle(handle);
    h->coordinator().setSurfaceSize(64, 64);
    h->coordinator().setSource(dataSource(fixture()));

    PlayerEvent ev;
    CHECK(waitForEvent(h, ev));
    CHECK(ev.type == PlayerEvent::Type::Loaded);
    CHECK(ev.metadata.width == 64 && ev.metadata.height == 64);

    h->playback().onLoaded(ev.metadata);
    CHECK(h->playback().state() == PlaybackState::Ready);

    h->coordinator().requestFrame(0, h->coordinator().generation());
    CHECK(waitForFrame(h));
    CHECK(h->sinkState().lastPublishedGeneration.load() == h->coordinator().generation());

    // The UI thread clears the flag after presenting.
    h->sinkState().newFrame.store(false, std::memory_order_release);
    CHECK(!h->sinkState().newFrame.load(std::memory_order_acquire));

    CHECK(destroyPlayerHandle(handle));
}

TEST(jni_event_queue_is_bounded) {
    SinkState state;
    AndroidFrameSink sink(state);
    PlayerEvent ev;
    ev.type = PlayerEvent::Type::Error;
    for (std::size_t i = 0; i < SinkState::kMaxQueuedEvents * 4; ++i) {
        ev.error = PlayerError{PlayerErrorCode::RenderFailed, std::to_string(i), 0};
        sink.onEvent(ev);
    }
    CHECK(state.events.size() == SinkState::kMaxQueuedEvents);
    // Oldest dropped, newest kept.
    PlayerEvent first;
    CHECK(popEvent(state, first));
    CHECK(first.error.message ==
          std::to_string(SinkState::kMaxQueuedEvents * 4 - SinkState::kMaxQueuedEvents));

    PlayerEvent drained;
    while (popEvent(state, drained)) {
    }
    CHECK(!popEvent(state, drained));
}

// --- Pixel conversion (the Bitmap blit) -------------------------------------

TEST(jni_pixel_swap_channels) {
    // rlottie word 0xAARRGGBB -> Android word 0xAABBGGRR.
    CHECK(rlottiePixelToAndroid(0xFFFF0000u) == 0xFF0000FFu);  // opaque red
    CHECK(rlottiePixelToAndroid(0xFF0000FFu) == 0xFFFF0000u);  // opaque blue
    CHECK(rlottiePixelToAndroid(0xFF00FF00u) == 0xFF00FF00u);  // green unchanged
    CHECK(rlottiePixelToAndroid(0x00000000u) == 0x00000000u);
    CHECK(rlottiePixelToAndroid(0x7F7F0000u) == 0x7F00007Fu);  // premultiplied half-red
    // Involutive: applying it twice is the identity.
    CHECK(rlottiePixelToAndroid(rlottiePixelToAndroid(0x12345678u)) == 0x12345678u);
}

// Converts the Chunk 0.4 golden (raw rlottie ARGB32 bytes) into an ARGB_8888
// surface with a WIDER stride, the way a real Bitmap is laid out, and checks
// every byte lands as R,G,B,A.
TEST(jni_golden_to_bitmap_bytes) {
    constexpr std::size_t kW = 64, kH = 64;
    const auto golden = tst::readBinary(tst::dataPath("golden/pixel-probe-frame0.argb32.raw"));
    CHECK(golden.size() == kW * kH * 4);
    if (golden.size() != kW * kH * 4) return;

    const std::size_t dstStride = (kW + 7) * 4;  // Bitmap stride > width * 4
    std::vector<std::uint8_t> bitmap(dstStride * kH, 0xCD);
    rlottieSurfaceToAndroid(golden.data(), kW * 4, bitmap.data(), dstStride, kW, kH);

    for (std::size_t y = 0; y < kH; ++y) {
        for (std::size_t x = 0; x < kW; ++x) {
            const std::uint8_t* s = golden.data() + (y * kW + x) * 4;  // B,G,R,A
            const std::uint8_t* d = bitmap.data() + y * dstStride + x * 4;  // R,G,B,A
            CHECK(d[0] == s[kByteOffsetRed]);
            CHECK(d[1] == s[kByteOffsetGreen]);
            CHECK(d[2] == s[kByteOffsetBlue]);
            CHECK(d[3] == s[kByteOffsetAlpha]);
        }
        // Stride padding is never written.
        for (std::size_t b = kW * 4; b < dstStride; ++b) {
            CHECK(bitmap[y * dstStride + b] == 0xCD);
        }
    }
}

// End-to-end: load + render through the handle, then blit — the exact path
// nativeCopyFrontInto takes — and compare against the golden.
TEST(jni_rendered_frame_matches_golden_after_blit) {
    constexpr std::size_t kW = 64, kH = 64;
    const std::int64_t handle = createPlayerHandle(kMaxPixels);
    auto* h = lookupPlayerHandle(handle);
    h->coordinator().setSurfaceSize(kW, kH);
    h->coordinator().setSource(dataSource(fixture()));

    PlayerEvent ev;
    CHECK(waitForEvent(h, ev));
    CHECK(ev.type == PlayerEvent::Type::Loaded);

    h->coordinator().requestFrame(0, h->coordinator().generation());
    CHECK(waitForFrame(h));

    const auto front = h->coordinator().frameBuffer().readFront();
    CHECK(front.pixels != nullptr);
    CHECK(front.width == kW && front.height == kH);

    std::vector<std::uint8_t> bitmap(kW * kH * 4, 0);
    rlottieSurfaceToAndroid(front.pixels, front.bytesPerLine, bitmap.data(), kW * 4, kW, kH);

    const auto golden = tst::readBinary(tst::dataPath("golden/pixel-probe-frame0.argb32.raw"));
    CHECK(golden.size() == bitmap.size());
    if (golden.size() == bitmap.size()) {
        std::vector<std::uint8_t> expected(golden.size());
        rlottieSurfaceToAndroid(golden.data(), kW * 4, expected.data(), kW * 4, kW, kH);
        CHECK(std::memcmp(bitmap.data(), expected.data(), expected.size()) == 0);
    }

    CHECK(destroyPlayerHandle(handle));
}

// --- Event wire format (parsed by RlottieView in Chunk 3.2) -----------------

TEST(jni_encode_loaded_event) {
    PlayerEvent ev;
    ev.type = PlayerEvent::Type::Loaded;
    ev.metadata.width = 64;
    ev.metadata.height = 48;
    ev.metadata.frameRate = 30.0;
    ev.metadata.totalFrames = 31;
    ev.metadata.duration = 1.0;
    const std::string encoded = encodeEvent(ev);
    CHECK(encoded.rfind("loaded:64,48,", 0) == 0);
    CHECK(encoded.find(",31,") != std::string::npos);
    CHECK(encoded.back() == '0');  // markerCount == 0
}

TEST(jni_encode_loaded_event_sanitizes_markers) {
    PlayerEvent ev;
    ev.type = PlayerEvent::Type::Loaded;
    ev.metadata.totalFrames = 10;
    ev.metadata.markers.push_back(Marker{"bad,name\nwith:stuff", 1, 5});
    const std::string encoded = encodeEvent(ev);
    CHECK(encoded.find("bad name with:stuff") != std::string::npos);
    CHECK(encoded.find('\n') == std::string::npos);
    // markerCount + the three marker fields are the only commas after it.
    CHECK(encoded.find(",1,5") != std::string::npos);
}

// Parity: the code strings MUST match the iOS adapter byte-for-byte, so they
// come from cpp/ErrorCode.h and nowhere else (plan §20/§21).
TEST(jni_encode_error_event_uses_canonical_codes) {
    PlayerEvent ev;
    ev.type = PlayerEvent::Type::Error;
    ev.error = PlayerError{PlayerErrorCode::ParseFailed, "boom\nnext", 0};
    CHECK(encodeEvent(ev) == "error:PARSE_FAILED:boom next");

    ev.error = PlayerError{PlayerErrorCode::InvalidDimensions, "", 0};
    CHECK(encodeEvent(ev) == "error:INVALID_DIMENSIONS:");

    ev.error = PlayerError{PlayerErrorCode::Released, "", 0};
    CHECK(encodeEvent(ev) == "error:RELEASED:");

    ev.error = PlayerError{PlayerErrorCode::SourceTooLarge, "a:b:c", 0};
    CHECK(encodeEvent(ev) == "error:SOURCE_TOO_LARGE:a:b:c");  // message is the tail
}

// --- Concurrency: the UI thread polls while the worker publishes ------------

TEST(jni_concurrent_poll_and_render) {
    const std::int64_t handle = createPlayerHandle(kMaxPixels);
    auto* h = lookupPlayerHandle(handle);
    h->coordinator().setSurfaceSize(64, 64);
    h->coordinator().setSource(dataSource(fixture()));

    std::atomic<bool> run{true};
    std::vector<std::uint8_t> bitmap(64 * 64 * 4, 0);
    std::thread ui([&] {
        std::size_t frame = 0;
        while (run.load()) {
            // Exactly what the Choreographer tick does, minus JNI.
            h->coordinator().requestFrame((frame++) % 31, h->coordinator().generation());
            if (h->sinkState().newFrame.exchange(false, std::memory_order_acq_rel)) {
                const auto front = h->coordinator().frameBuffer().readFront();
                if (front.pixels != nullptr) {
                    rlottieSurfaceToAndroid(front.pixels, front.bytesPerLine, bitmap.data(),
                                            front.width * 4, front.width, front.height);
                }
            }
            PlayerEvent ev;
            while (popEvent(h->sinkState(), ev)) {
                if (ev.type == PlayerEvent::Type::Loaded) h->playback().onLoaded(ev.metadata);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    run.store(false);
    ui.join();
    CHECK(destroyPlayerHandle(handle));
}
