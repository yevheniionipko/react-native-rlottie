// Chunk 3.1 — AndroidFrameSink implementation. Runs on the render worker; only
// records state (no JVM interaction).
#include "AndroidFrameSink.h"

namespace rnrlottie {

void AndroidFrameSink::onFramePublished(std::uint64_t generation) {
    state_.lastPublishedGeneration.store(generation, std::memory_order_release);
    state_.newFrame.store(true, std::memory_order_release);
}

bool popEvent(SinkState& state, PlayerEvent& out) {
    std::lock_guard<std::mutex> lk(state.eventMutex);
    if (state.events.empty()) {
        return false;
    }
    out = state.events.front();
    state.events.pop_front();
    return true;
}

void AndroidFrameSink::onEvent(const PlayerEvent& event) {
    std::lock_guard<std::mutex> lk(state_.eventMutex);
    if (state_.events.size() >= SinkState::kMaxQueuedEvents) {
        state_.events.pop_front();  // drop the oldest rather than grow unbounded
    }
    state_.events.push_back(event);
}

}  // namespace rnrlottie
