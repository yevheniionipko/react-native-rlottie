// Chunk 0.3 — toolchain probe.
//
// A trivial exported symbol proving the vendored rlottie static library links
// on each platform. Not part of the shipping API; the real core arrives in
// Phase 1. Kept in cpp/spike/ so it can be dropped once Phase 1 lands.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Returns the vendored rlottie commit SHA (rnrlottie::kRlottieCommit).
// Referencing a real rlottie symbol in the implementation forces the linker to
// resolve librlottie, so a successful link of a caller proves the wiring works.
const char* rnrlottieProbe(void);

#ifdef __cplusplus
}
#endif
