// Chunk 1.5 — the platform callback interface implemented by each adapter.
//
// The core never references a platform type; it talks to the platform only
// through this injected interface. Both methods are invoked ON THE RENDER
// WORKER; the platform must marshal presentation/event delivery to its UI
// thread and keep the work cheap.
#pragma once

#include <cstdint>

#include "AnimationMetadata.h"
#include "PlayerError.h"

namespace rnrlottie {

struct PlayerEvent {
    enum class Type { Loaded, Error };
    Type type = Type::Loaded;
    AnimationMetadata metadata;  // valid when type == Loaded
    PlayerError error;           // valid when type == Error
};

class FrameSink {
public:
    virtual ~FrameSink() = default;

    // [worker] A frame was published to the front buffer for `generation`.
    virtual void onFramePublished(std::uint64_t generation) = 0;

    // [worker] A load result (Loaded) or a render/resize error (Error).
    virtual void onEvent(const PlayerEvent& event) = 0;
};

}  // namespace rnrlottie
