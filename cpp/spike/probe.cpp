// Chunk 0.3 — toolchain probe implementation. See probe.h.
#include "probe.h"

#include <rlottie.h>

#include "RlottieVersion.h"

extern "C" const char* rnrlottieProbe(void) {
    // Force the linker to resolve a real rlottie symbol WITHOUT mutating global
    // state: take the address of configureModelCacheSize (a real exported
    // global) behind a volatile sink so it cannot be optimized away.
    void (*volatile fn)(size_t) = &rlottie::configureModelCacheSize;
    (void)fn;
    return rnrlottie::kRlottieCommit;
}
