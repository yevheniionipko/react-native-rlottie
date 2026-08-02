// Committed replacement for rlottie's CMake-generated config.h (Chunk 0.3).
//
// Several rlottie sources `#include "config.h"`. rlottie's CMake normally
// generates it from cmake/config.h.in into the build dir. The iOS podspec
// builds rlottie sources directly (no CMake), so we supply this file on the
// header search path instead. It mirrors config.h.in with the plan §14 options:
//   LOTTIE_THREAD = ON   -> LOTTIE_THREAD_SUPPORT
//   LOTTIE_CACHE  = ON   -> LOTTIE_CACHE_SUPPORT
//   LOTTIE_MODULE = OFF  -> LOTTIE_IMAGE_MODULE_SUPPORT left undefined
//
// The Android build uses rlottie's own CMake (which generates its own config.h),
// so this file is iOS/host-only.
#pragma once

#define LOTTIE_IMAGE_MODULE_PLUGIN ""

#define LOTTIE_THREAD_SUPPORT

#define LOTTIE_CACHE_SUPPORT
