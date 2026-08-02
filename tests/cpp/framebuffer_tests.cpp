// Chunk 1.7 — FrameBuffer semantics + double-buffer concurrency contract (§0.3).
#include <atomic>
#include <cstdint>
#include <thread>

#include "FrameBuffer.h"
#include "test_harness.h"

using namespace rnrlottie;

TEST(framebuffer_resize_validation) {
    FrameBuffer fb(FrameBuffer::Limits{/*maxPixels=*/1024});
    PlayerError err;
    CHECK(!fb.resize({0, 10}, err) && err.code == PlayerErrorCode::InvalidDimensions);
    CHECK(!fb.resize({33, 33}, err) && err.code == PlayerErrorCode::InvalidDimensions);
    CHECK(!fb.resize({SIZE_MAX, 2}, err) && err.code == PlayerErrorCode::InvalidDimensions);
    CHECK(!fb.isSized());
    CHECK(fb.acquireRenderTarget() == nullptr);
    CHECK(fb.readFront().pixels == nullptr);
}

TEST(framebuffer_double_buffer_cycle) {
    FrameBuffer fb(FrameBuffer::Limits{1024});
    PlayerError err;
    CHECK(fb.resize({32, 32}, err));
    CHECK(fb.bytesPerLine() == 128);
    CHECK(fb.readFront().pixels == nullptr);  // nothing published

    std::uint32_t* backA = fb.acquireRenderTarget();
    CHECK(fb.resize({32, 32}, err));                 // idempotent
    CHECK(fb.acquireRenderTarget() == backA);

    for (std::size_t i = 0; i < 32 * 32; ++i) backA[i] = 0xAABBCCDD;
    fb.publish();
    auto f0 = fb.readFront();
    CHECK(f0.pixels == backA && f0.width == 32 && f0.bytesPerLine == 128);
    CHECK(f0.pixels[0] == 0xAABBCCDD && f0.pixels[32 * 32 - 1] == 0xAABBCCDD);

    std::uint32_t* backB = fb.acquireRenderTarget();
    CHECK(backB != backA);  // double buffer alternation
    backB[0] = 0x11223344;
    fb.publish();
    CHECK(fb.readFront().pixels == backB);

    CHECK(fb.resize({16, 16}, err));               // genuine resize invalidates front
    CHECK(fb.readFront().pixels == nullptr);
}

namespace {
struct SpinBarrier {
    std::atomic<int> count{0};
    std::atomic<int> gen{0};
    void wait() {
        const int g = gen.load(std::memory_order_acquire);
        if (count.fetch_add(1, std::memory_order_acq_rel) + 1 == 2) {
            count.store(0, std::memory_order_relaxed);
            gen.fetch_add(1, std::memory_order_release);
        } else {
            while (gen.load(std::memory_order_acquire) == g) std::this_thread::yield();
        }
    }
};
}  // namespace

// Worker fills the BACK buffer while main reads the FRONT buffer concurrently
// (disjoint), then a barrier, then the worker publishes. Run under TSan.
TEST(framebuffer_concurrency_no_tearing) {
    constexpr std::size_t W = 32, H = 32, N = 20000;
    FrameBuffer fb(FrameBuffer::Limits{1u << 20});
    PlayerError err;
    CHECK(fb.resize({W, H}, err));

    std::uint32_t v = 1;
    for (std::uint32_t* t = fb.acquireRenderTarget(); t;) {
        for (std::size_t i = 0; i < W * H; ++i) t[i] = v;
        fb.publish();
        break;
    }

    SpinBarrier b;
    std::atomic<int> torn{0};
    std::thread main([&] {
        for (std::size_t i = 0; i < N; ++i) {
            auto f = fb.readFront();
            if (!f.pixels || f.pixels[0] != f.pixels[W * H - 1]) torn.fetch_add(1);
            b.wait();
            b.wait();
        }
    });
    std::thread worker([&] {
        for (std::size_t i = 0; i < N; ++i) {
            std::uint32_t* t = fb.acquireRenderTarget();
            const std::uint32_t nv = v + 1;
            for (std::size_t k = 0; k < W * H; ++k) t[k] = nv;
            b.wait();
            fb.publish();
            v = nv;
            b.wait();
        }
    });
    worker.join();
    main.join();
    CHECK(torn.load() == 0);
}
