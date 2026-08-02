// Chunk 1.5 — the synchronization boundary (the concrete "RenderCoordinator").
//
// Owns the per-view render worker thread, the authoritative generation token,
// the RlottiePlayerCore, and the FrameBuffer. Its public API is thread-safe
// ([any] thread) and internally hops onto the serial worker, which is what makes
// "the same animation is never rendered and mutated concurrently" structural.
//
// Scheduling (plan §0.3): renders are single-in-flight and latest-frame-wins via
// a pending-frame slot (NOT a growing queue). Structural changes (source, size,
// reset, release) bump the generation synchronously on the caller thread so the
// view can reset PlaybackController in lock-step; the work is enqueued and any
// result whose captured generation is stale is dropped rather than presented.
//
// Teardown: release() (also called by the destructor) bumps the generation,
// cancels pending work, destroys the core ON THE WORKER, then joins — so NO sink
// callback fires after release() returns.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "AnimationSource.h"
#include "FrameBuffer.h"
#include "FrameSink.h"
#include "RlottiePlayerCore.h"

namespace rnrlottie {

class RenderCoordinator final {
public:
    // `sink` must outlive the coordinator (the adapter guarantees this by calling
    // release() before destroying the sink).
    RenderCoordinator(FrameSink& sink, FrameBuffer::Limits limits);
    ~RenderCoordinator();

    RenderCoordinator(const RenderCoordinator&) = delete;
    RenderCoordinator& operator=(const RenderCoordinator&) = delete;

    // Structural changes — bump the generation (returned) and enqueue work.
    std::uint64_t setSource(AnimationSource source);
    std::uint64_t setSurfaceSize(std::size_t width, std::size_t height);

    // Request a render of `frame` tagged with the generation the caller observed.
    // Single-in-flight: overwrites the pending slot if a render is outstanding.
    void requestFrame(std::size_t frame, std::uint64_t generation);

    void setColor(std::string keyPath, float red, float green, float blue);
    // Chunk 6.1 — same routing/generation-gating shape as setColor: enqueued
    // onto the control queue, gated on the source epoch captured at call time
    // so a stale keyPath belonging to a replaced source is dropped rather than
    // applied to the new animation.
    void setOpacity(std::string keyPath, float opacity);
    void setStrokeWidth(std::string keyPath, float width);

    // Invalidate in-flight work (bumps generation, clears the pending slot).
    void reset();

    // Idempotent. Cancels work, destroys the core on the worker, joins.
    void release();

    const FrameBuffer& frameBuffer() const;  // [main] presenter reads front
    std::uint64_t generation() const;

private:
    void workerLoop();
    void runLoad(AnimationSource source, std::uint64_t sourceEpoch);
    void runResize(std::size_t width, std::size_t height, std::uint64_t generation);
    void runRender(std::size_t frame, std::uint64_t generation);
    void runSetColor(std::string keyPath, float r, float g, float b,
                     std::uint64_t sourceEpoch);
    void runSetOpacity(std::string keyPath, float opacity, std::uint64_t sourceEpoch);
    void runSetStrokeWidth(std::string keyPath, float width, std::uint64_t sourceEpoch);

    FrameSink& sink_;
    FrameBuffer frameBuffer_;
    std::unique_ptr<RlottiePlayerCore> core_;
    // Global generation: bumped by source/size/reset/release; gates stale RENDER
    // results and is what the view passes to requestFrame().
    std::atomic<std::uint64_t> generation_{0};
    // Source epoch: bumped ONLY by setSource; gates stale parses/Loaded events so
    // a later resize does not suppress a valid load.
    std::atomic<std::uint64_t> sourceEpoch_{0};

    std::mutex mutex_;
    std::condition_variable cv_;
    // load/resize/setColor/setOpacity/setStrokeWidth
    std::deque<std::function<void()>> controlQueue_;
    std::optional<std::size_t> pendingFrame_;         // latest-wins render slot
    std::uint64_t pendingGeneration_ = 0;
    bool stop_ = false;
    bool released_ = false;

    std::thread worker_;  // declared last; started in the constructor body
};

}  // namespace rnrlottie
