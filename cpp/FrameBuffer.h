// Chunk 1.3 — reusable double-buffered pixel memory.
//
// Holds two premultiplied-ARGB32 buffers (front = presented, back = rendering).
// No per-frame allocation: buffers are reused until pixel dimensions change
// (plan §7). Overflow-checked, clamped to a max pixel area.
//
// Concurrency contract:
//   * STEADY STATE (fixed dims): acquireRenderTarget()+publish() on [worker] run
//     concurrently with readFront() on [main]. This is race-free via the atomic
//     frontIndex_ (release on publish / acquire on read), which also establishes
//     happens-before for the rendered pixels. This is the double-buffer safety
//     rule from §0.3: a buffer handed to the presenter is not reused as a render
//     target until the next frame is requested (single-in-flight, tick-driven).
//   * resize() is a STRUCTURAL change and must NOT run concurrently with
//     readFront(). The owner (RenderCoordinator / the view) serializes it via a
//     generation bump: stop presenting, resize, resume (plan §7 "keep displaying
//     the last valid frame until the new buffer is ready"). dims_ is therefore
//     stable during the steady-state concurrent phase.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "PlayerError.h"

namespace rnrlottie {

class FrameBuffer final {
public:
    struct Dimensions {
        std::size_t width = 0;
        std::size_t height = 0;
    };

    struct Limits {
        std::size_t maxPixels = 4096u * 4096u;  // clamp (overridden via Chunk 1.6)
    };

    // A consistent snapshot of the presented buffer for the presenter [main].
    // pixels == nullptr when nothing has been published for the current dims.
    struct FrontBuffer {
        const std::uint32_t* pixels = nullptr;
        std::size_t width = 0;
        std::size_t height = 0;
        std::size_t bytesPerLine = 0;
    };

    explicit FrameBuffer(Limits limits);

    // [worker] Ensure both buffers match dims; reallocates only on change and is
    // idempotent for equal dims (preserving the current front). Rejects zero,
    // out-of-range (> UINT32_MAX per axis), multiplication-overflowing, or
    // > maxPixels dims with false + outError (InvalidDimensions), and a genuine
    // allocation failure with AllocationFailed. On rejection the previous buffer
    // state is left intact. On a successful change the front is invalidated
    // until the next publish() and both buffers are cleared to transparent.
    bool resize(Dimensions dims, PlayerError& outError);

    // [worker] The buffer to render INTO (the back buffer). null if unsized.
    std::uint32_t* acquireRenderTarget();

    // [worker] Current render dimensions / scanline stride.
    Dimensions dimensions() const;
    std::size_t bytesPerLine() const;  // width * 4

    // [worker] Promote the just-rendered back buffer to front (swap indices).
    void publish();

    // [main] Consistent snapshot of the presented buffer (see FrontBuffer).
    FrontBuffer readFront() const;

    bool isSized() const;

    // [any thread] Chunk 7.1 instrumentation. Tracked unconditionally (not
    // gated by RenderCoordinator's metricsEnabled flag): resize() is a
    // structural, dimension-change event — not a per-frame hot path — so
    // there is no "cost nothing when disabled" requirement to satisfy here,
    // and having these always-on means bufferAllocCount is a reliable §21
    // acceptance signal ("no per-frame native memory allocation") independent
    // of whether the opt-in metrics feature is on. Written only from
    // [worker] inside resize(); safe to read from [any] thread (relaxed
    // atomics — these are independent point-in-time counters, not part of a
    // larger transaction).
    //
    // allocCount(): incremented once per resize() call that actually
    // (re)allocates, i.e. NOT the idempotent equal-dims early-return and NOT
    // a rejected (invalid-dims / OOM) call. In steady state (fixed layout
    // dimensions) this stops increasing, which is exactly the invariant
    // tests/cpp asserts.
    std::uint64_t allocCount() const { return allocCount_.load(std::memory_order_relaxed); }

    // peakBytes(): high-water mark of this FrameBuffer's total allocation
    // (front + back buffer combined, width*height*4 bytes each). This is the
    // view's OWN buffer memory, not process RSS — see docs/bridge-contract.md
    // for why that distinction is deliberate.
    std::uint64_t peakBytes() const { return peakBytes_.load(std::memory_order_relaxed); }

private:
    std::array<std::vector<std::uint32_t>, 2> buffers_;
    std::atomic<int> frontIndex_{-1};  // -1 = nothing published; else 0/1
    int backIndex_ = 0;                // [worker]-only
    Dimensions dims_;                  // [worker]-authoritative
    Limits limits_;
    std::atomic<std::uint64_t> allocCount_{0};
    std::atomic<std::uint64_t> peakBytes_{0};
};

}  // namespace rnrlottie
