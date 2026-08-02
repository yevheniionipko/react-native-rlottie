// Chunk 3.1 — JniPlayerHandle implementation.
#include "JniPlayerHandle.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

namespace rnrlottie {
namespace {

// Function-local statics: initialized on first use, no static-init order issues
// with JNI_OnLoad, and never destroyed (a player outliving main() is impossible
// on Android, and skipping teardown avoids exit-time races).
struct Registry {
    std::mutex mutex;
    std::int64_t nextId = 1;  // 0 is the reserved "no handle" value
    std::unordered_map<std::int64_t, std::unique_ptr<JniPlayerHandle>> players;
};

Registry& registry() {
    static Registry* r = new Registry();
    return *r;
}

}  // namespace

JniPlayerHandle::JniPlayerHandle(std::size_t maxPixels)
    : sink_(sinkState_),
      coordinator_(sink_, FrameBuffer::Limits{maxPixels}) {}

// coordinator_'s destructor calls release() (joins the worker) before sink_ and
// sinkState_ are torn down, thanks to reverse-order member destruction.
JniPlayerHandle::~JniPlayerHandle() = default;

std::int64_t createPlayerHandle(std::size_t maxPixels) {
    const std::size_t limit = maxPixels > 0 ? maxPixels : FrameBuffer::Limits{}.maxPixels;
    // Constructed outside the lock: the constructor starts a render worker.
    std::unique_ptr<JniPlayerHandle> player(new (std::nothrow) JniPlayerHandle(limit));
    if (!player) {
        return 0;
    }
    auto& reg = registry();
    std::lock_guard<std::mutex> lk(reg.mutex);
    const std::int64_t id = reg.nextId++;  // monotonic; ids are never reused
    reg.players.emplace(id, std::move(player));
    return id;
}

JniPlayerHandle* lookupPlayerHandle(std::int64_t id) {
    if (id <= 0) {
        return nullptr;
    }
    auto& reg = registry();
    std::lock_guard<std::mutex> lk(reg.mutex);
    const auto it = reg.players.find(id);
    if (it == reg.players.end()) {
        return nullptr;  // unknown, forged, or already destroyed
    }
    JniPlayerHandle* player = it->second.get();
    return player->valid() ? player : nullptr;
}

bool destroyPlayerHandle(std::int64_t id) {
    if (id <= 0) {
        return false;
    }
    std::unique_ptr<JniPlayerHandle> player;
    {
        auto& reg = registry();
        std::lock_guard<std::mutex> lk(reg.mutex);
        const auto it = reg.players.find(id);
        if (it == reg.players.end()) {
            return false;  // idempotent: unknown or already destroyed
        }
        player = std::move(it->second);
        reg.players.erase(it);
    }
    // Outside the lock: release() joins the render worker, which can take as long
    // as one in-flight frame — no other view's JNI call should block on that.
    // It cancels pending work and destroys the core ON THE WORKER, so no sink
    // callback can still be in flight when it returns.
    player->coordinator().release();
    player->invalidate();
    return true;  // ~unique_ptr frees it here
}

std::size_t livePlayerCount() {
    auto& reg = registry();
    std::lock_guard<std::mutex> lk(reg.mutex);
    return reg.players.size();
}

}  // namespace rnrlottie
