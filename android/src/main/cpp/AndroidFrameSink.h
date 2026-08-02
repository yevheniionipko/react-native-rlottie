// Chunk 3.1 — Android FrameSink: a POLL-BASED sink.
//
// FrameSink callbacks run on the RenderCoordinator's render worker thread. To
// avoid attaching the worker to the JVM (and the lifetime hazards of a
// worker->Java call), this sink NEVER touches the JVM. It only records state:
//   * an atomic "new frame" flag + the latest published generation, and
//   * a small, mutex-guarded queue of load/error events.
// The Kotlin view drains both on the UI thread every Choreographer tick
// (nativeHasNewFrame / nativeCopyFrontInto / nativePollEvent).
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

#include "FrameSink.h"

namespace rnrlottie {

// Shared state written by the worker (via the sink) and polled by the UI thread.
struct SinkState {
    std::atomic<bool> newFrame{false};
    std::atomic<std::uint64_t> lastPublishedGeneration{0};

    std::mutex eventMutex;
    std::deque<PlayerEvent> events;
    // Bound the queue so a view that never polls cannot grow it without limit.
    static constexpr std::size_t kMaxQueuedEvents = 64;
};

// [UI] Pops the oldest queued event. Returns false when the queue is empty.
bool popEvent(SinkState& state, PlayerEvent& out);

class AndroidFrameSink final : public FrameSink {
public:
    explicit AndroidFrameSink(SinkState& state) : state_(state) {}

    void onFramePublished(std::uint64_t generation) override;
    void onEvent(const PlayerEvent& event) override;

private:
    SinkState& state_;
};

}  // namespace rnrlottie
