// Chunk 1.1 — the single playback state enum.
//
// Owned and mutated only by PlaybackController on the [main] thread (Chunk 1.4).
// Playback is modeled as ONE state with explicit transitions, never as several
// unrelated booleans (plan §4). Transition map (plan §4):
//
//   Empty --load--> Loading --success--> Ready --play--> Playing <--> Paused
//                     |                                      |
//                     +--failure--> Failed          stop --> Stopped
//   any --source changed--> Loading      any --view detached--> Released
#pragma once

namespace rnrlottie {

enum class PlaybackState {
    Empty,
    Loading,
    Ready,
    Playing,
    Paused,
    Stopped,
    Failed,
    Released,
};

}  // namespace rnrlottie
