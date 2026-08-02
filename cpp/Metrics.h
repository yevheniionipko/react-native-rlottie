// Chunk 7.1 — opt-in instrumentation for a single RenderCoordinator/view.
//
// Frozen contract (docs/bridge-contract.md, "Phase 7 additions —
// instrumentation"): MetricsSnapshot's shape and RenderCoordinator's
// setMetricsEnabled()/metricsSnapshot() signatures are fixed; implement them
// exactly as specified there.
//
// Header-only (like InputLimits.h): both tests/cpp/CMakeLists.txt and
// android/src/main/cpp/CMakeLists.txt enumerate cpp/*.cpp explicitly rather
// than globbing, and this agent does not own the Android CMakeLists, so a new
// .cpp is avoided. A C++17 inline-function-local static would give one
// process-wide instance if that pattern were needed here, exactly as in
// InputLimits.h, but MetricsCollector is a plain per-RenderCoordinator member
// object (not global state), so it needs no such trick — just `inline` on any
// free functions to satisfy ODR across translation units.
//
// Threading contract:
//   * Writes (recordParseMs / markSourceSet / recordFramePublished /
//     recordFrameDropped) happen ONLY on the render [worker] thread, called
//     from RenderCoordinator's runLoad()/setSource()/runRender()/
//     requestFrame(). (setSource()'s markSourceSet() call is the one
//     exception — it runs on the CALLER thread, same as the rest of
//     setSource() — see the cross-thread note on markSourceSet() below.)
//   * Reads (snapshot()) happen from the UI thread via
//     RenderCoordinator::metricsSnapshot(), concurrently with the writes
//     above. Every field is either a std::atomic with relaxed ordering (plain
//     counters/gauges, where only the eventual value matters, not causality
//     with other fields) or protected by renderWindowMutex_ (the ring buffer,
//     which needs true mutual exclusion because computing a percentile reads
//     the whole array while a concurrent write mutates one slot). This is
//     deliberately not lock-free-if-possible: correctness and a clean TSan run
//     matter more here than shaving a mutex off a once-per-frame, opt-in path.
//
// Disabled-path cost (the hard requirement — "must cost effectively nothing
// per frame"): every recording method starts with `if (!enabled()) return;`,
// i.e. one relaxed atomic load and a predicted-taken branch. No clock read, no
// allocation, no lock is reachable when disabled. Callers additionally gate
// their OWN clock reads on enabled() first (see RenderCoordinator.cpp) so a
// disabled collector doesn't even pay for a steady_clock::now() that would
// just be thrown away.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace rnrlottie {

// The exact shape frozen by docs/bridge-contract.md. Do not reorder/rename —
// the platform layers (owned by other agents/streams) bind to this directly.
struct MetricsSnapshot {
    double parseMs = 0.0;
    double firstFrameMs = 0.0;
    double renderP50Ms = 0.0;
    double renderP95Ms = 0.0;
    double renderP99Ms = 0.0;
    std::uint64_t framesRendered = 0;
    std::uint64_t framesDropped = 0;
    std::uint64_t bufferAllocCount = 0;
    std::uint64_t peakBufferBytes = 0;
};

// The monotonic clock every duration in this file is measured against
// (CLAUDE.md: "never wall clock — a user changing the device clock must not
// produce negative or absurd timings").
using MetricsClock = std::chrono::steady_clock;

// Per-RenderCoordinator instrumentation. Bounded memory, always: nothing in
// this type ever grows with uptime or frame count.
//
// bufferAllocCount/peakBufferBytes are NOT tracked here — FrameBuffer tracks
// its own (re)allocation count and peak byte high-water-mark directly (it
// already owns dims_/buffers_, so it is the single source of truth for
// whether a resize() call actually reallocated). RenderCoordinator::
// metricsSnapshot() merges FrameBuffer's counters into the snapshot this type
// produces. See FrameBuffer.h.
class MetricsCollector final {
public:
    // Rolling window capacity for render-duration samples. Fixed-size
    // std::array, never resized, never heap-allocated: the entire collector
    // is exactly sizeof(MetricsCollector) bytes regardless of how many frames
    // a long-running view renders — an unbounded sample vector would itself
    // be a leak (the whole reason this is a ring buffer and not a
    // std::vector<double> that grows forever).
    //
    // Accuracy trade-off accepted: percentiles are computed over only the
    // most recent kRenderWindowCapacity render samples, not the view's full
    // lifetime. At 512 samples this is a few seconds of history at typical
    // 30-120fps render rates, which is exactly the "is it currently janky"
    // window a metrics consumer cares about (throttled to at most 1
    // onMetrics event/sec anyway, per the bridge contract) — a lifetime
    // average would actually be a worse answer to "how is this view
    // rendering right now". Fixed cost: 512 * sizeof(double) = 4 KiB.
    static constexpr std::size_t kRenderWindowCapacity = 512;

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // [worker] Call once after a load completes successfully, with the wall
    // time the parse itself took.
    void recordParseMs(double ms) {
        if (!enabled()) return;
        parseMs_.store(ms, std::memory_order_relaxed);
    }

    // Arms first-frame timing: the next recordFramePublished() after this call
    // computes "source set -> first frame published" and latches it into
    // firstFrameMs. Called from setSource(), which per RenderCoordinator's
    // contract is callable from [any] thread — so unlike every other writer
    // here, this one is NOT necessarily the render worker. sourceSetAtNs_ is
    // published with release ordering and consumed with acquire ordering in
    // recordFramePublished() (via the CAS on firstFrameArmed_), which is what
    // makes this safe without a full lock: whichever thread eventually flips
    // firstFrameArmed_ from true to false is guaranteed to see this store.
    void markSourceSet() {
        if (!enabled()) return;
        sourceSetAtNs_.store(nowNs(), std::memory_order_relaxed);
        firstFrameArmed_.store(true, std::memory_order_release);
    }

    // [worker] Call after a frame is actually rendered AND published (not
    // coalesced away, not stale). `renderMs` is the wall time the render call
    // itself took.
    void recordFramePublished(double renderMs) {
        if (!enabled()) return;
        framesRendered_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(renderWindowMutex_);
            renderSamples_[renderWriteIndex_] = renderMs;
            renderWriteIndex_ = (renderWriteIndex_ + 1) % kRenderWindowCapacity;
            if (renderCount_ < kRenderWindowCapacity) ++renderCount_;
        }
        bool expected = true;
        if (firstFrameArmed_.compare_exchange_strong(expected, false,
                                                      std::memory_order_acquire)) {
            const std::int64_t startNs = sourceSetAtNs_.load(std::memory_order_relaxed);
            const double ms = static_cast<double>(nowNs() - startNs) / 1e6;
            firstFrameMs_.store(ms, std::memory_order_relaxed);
        }
    }

    // [worker] A render request was coalesced away by latest-frame-wins
    // (RenderCoordinator::requestFrame overwrote a not-yet-serviced pending
    // frame). This is correct scheduling behaviour, not an error — see the
    // bridge contract.
    void recordFrameDropped() {
        if (!enabled()) return;
        framesDropped_.fetch_add(1, std::memory_order_relaxed);
    }

    // [any thread] A consistent-enough point-in-time read. Individual atomic
    // fields are relaxed (no ordering is needed between e.g. parseMs and
    // framesRendered — they are independent counters, not a transaction), and
    // the render-duration percentiles are computed under renderWindowMutex_
    // so a concurrent write can never be observed mid-sort or torn.
    // bufferAllocCount/peakBufferBytes are left at 0 here; the caller
    // (RenderCoordinator) fills them in from FrameBuffer.
    MetricsSnapshot snapshot() const {
        MetricsSnapshot s;
        s.parseMs = parseMs_.load(std::memory_order_relaxed);
        s.firstFrameMs = firstFrameMs_.load(std::memory_order_relaxed);
        s.framesRendered = framesRendered_.load(std::memory_order_relaxed);
        s.framesDropped = framesDropped_.load(std::memory_order_relaxed);

        std::array<double, kRenderWindowCapacity> copy{};
        std::size_t n = 0;
        {
            std::lock_guard<std::mutex> lk(renderWindowMutex_);
            n = renderCount_;
            copy = renderSamples_;
        }
        if (n > 0) {
            std::sort(copy.begin(), copy.begin() + static_cast<std::ptrdiff_t>(n));
            s.renderP50Ms = percentile(copy, n, 0.50);
            s.renderP95Ms = percentile(copy, n, 0.95);
            s.renderP99Ms = percentile(copy, n, 0.99);
        }
        return s;
    }

private:
    static std::int64_t nowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   MetricsClock::now().time_since_epoch())
            .count();
    }

    // Nearest-rank percentile over the first `n` entries of an ascending-
    // sorted array. p in [0,1]; index = floor(p * (n-1)), which is exact and
    // unambiguous at p=0 (min) and p=1 (max) and matches the common
    // "percentile of a finite sample" convention used by e.g. numpy's default
    // linear-adjacent variant collapsed to nearest observed sample (no
    // interpolation, since these are discrete render-duration observations,
    // not a continuous distribution).
    static double percentile(const std::array<double, kRenderWindowCapacity>& sorted,
                             std::size_t n, double p) {
        if (n == 0) return 0.0;
        std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(n - 1));
        if (idx >= n) idx = n - 1;
        return sorted[idx];
    }

    std::atomic<bool> enabled_{false};

    std::atomic<double> parseMs_{0.0};
    std::atomic<double> firstFrameMs_{0.0};
    std::atomic<bool> firstFrameArmed_{false};
    std::atomic<std::int64_t> sourceSetAtNs_{0};

    std::atomic<std::uint64_t> framesRendered_{0};
    std::atomic<std::uint64_t> framesDropped_{0};

    // Bounded rolling window of render durations (ms). Protected by
    // renderWindowMutex_ rather than made lock-free: writes are already
    // serialized (render worker only) and reads are infrequent (at most once
    // per onMetrics tick, throttled to 1/sec by the bridge layer), so a mutex
    // costs nothing measurable while keeping snapshot()'s sort-under-lock
    // trivially race-free.
    mutable std::mutex renderWindowMutex_;
    std::array<double, kRenderWindowCapacity> renderSamples_{};
    std::size_t renderWriteIndex_ = 0;
    std::size_t renderCount_ = 0;
};

}  // namespace rnrlottie
