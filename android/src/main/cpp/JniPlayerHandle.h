// Chunk 3.1 — the opaque native handle behind the Android jlong.
//
// Owns everything a single RlottieView needs natively: the RenderCoordinator
// (with its poll-based sink), the SinkState the UI thread polls, and the
// PlaybackController (accessed only from the UI thread via JNI).
//
// Handle safety: the jlong Kotlin holds is NOT a pointer — it is an opaque,
// never-reused id into a process-wide registry (see below). A stale, forged or
// double-destroyed id simply misses the map; nothing dereferences freed memory.
//
// A raw-pointer + magic-sentinel scheme was tried first and rejected: validating
// the sentinel after nativeDestroy *is* a use-after-free (ASan reports it), so
// it cannot meet the "double handle calls fail safe" criterion. `magic_` is kept
// only as cheap defense-in-depth for a live entry.
//
// Member order matters: magic_, then sinkState_, then sink_ (references
// sinkState_), then coordinator_ (references sink_), then playback_. Destruction
// runs in reverse, so coordinator_ (whose destructor calls release() and joins
// the worker) is torn down BEFORE the sink and its state — no callback can fire
// into freed memory.
#pragma once

#include <cstddef>
#include <cstdint>

#include "AndroidFrameSink.h"
#include "FrameBuffer.h"
#include "PlaybackController.h"
#include "RenderCoordinator.h"

namespace rnrlottie {

class JniPlayerHandle final {
public:
    // "RLOTTIE\0" as bytes.
    static constexpr std::uint64_t kMagic = 0x524C4F5454494500ULL;

    explicit JniPlayerHandle(std::size_t maxPixels);
    ~JniPlayerHandle();

    JniPlayerHandle(const JniPlayerHandle&) = delete;
    JniPlayerHandle& operator=(const JniPlayerHandle&) = delete;

    bool valid() const { return magic_ == kMagic; }
    void invalidate() { magic_ = 0; }

    RenderCoordinator& coordinator() { return coordinator_; }
    PlaybackController& playback() { return playback_; }
    SinkState& sinkState() { return sinkState_; }

private:
    std::uint64_t magic_ = kMagic;
    SinkState sinkState_;
    AndroidFrameSink sink_;
    RenderCoordinator coordinator_;
    PlaybackController playback_;
};

// --- The handle registry ----------------------------------------------------
//
// These functions ARE the handle-safety contract, and are deliberately JNI-free
// so the fault-injection tests (tests/cpp/android_tests.cpp) exercise the same
// code the JNI entry points use rather than a copy of it.
//
// The registry (a mutex-guarded id -> owning-pointer map) is the sole owner of
// every live player. Ids are minted from a monotonic counter and NEVER reused,
// so a stale id can never resolve to a different player.
//
// Threading: the map itself is safe from any thread. The returned raw pointer is
// only guaranteed live while the caller is the owner of the id — which the
// Kotlin layer guarantees by issuing all handle calls, including destroy, on the
// UI thread (see the contract at the top of RlottieJni.cpp).

// Creates a player and registers it. Returns 0 when allocation fails; never
// throws (the JNI boundary must not see an exception — plan §17).
std::int64_t createPlayerHandle(std::size_t maxPixels);

// Resolves an id. Returns nullptr for 0, an unknown/forged id, or one that has
// already been destroyed. Never dereferences freed memory.
JniPlayerHandle* lookupPlayerHandle(std::int64_t id);

// Unregisters the player, releases the coordinator (joins the worker) and frees
// it. Idempotent: an unknown/already-destroyed id is a no-op returning false.
bool destroyPlayerHandle(std::int64_t id);

// Live player count — teardown-leak assertions in tests.
std::size_t livePlayerCount();

}  // namespace rnrlottie
