// Chunk 3.1 — rlottie surface -> Android Bitmap.Config.ARGB_8888 conversion.
//
// rlottie renders premultiplied ARGB32 whose memory byte order is B,G,R,A
// (little-endian word 0xAARRGGBB — see cpp/PixelFormat.h / Chunk 0.4). Android's
// ARGB_8888 is R,G,B,A in native memory (little-endian word 0xAABBGGRR), also
// premultiplied. So exactly one thing differs: red and blue are swapped.
//
// Deliberately JNI-free (no <jni.h>, no <android/bitmap.h>) so the golden test
// in tests/cpp/android_tests.cpp exercises the same code the JNI blit uses.
#pragma once

#include <cstddef>
#include <cstdint>

#include "PixelFormat.h"

namespace rnrlottie {

// One pixel: swap the R (bits 16-23) and B (bits 0-7) lanes; A and G stay put.
inline std::uint32_t rlottiePixelToAndroid(std::uint32_t p) {
    if (!kAndroidNeedsChannelSwap) {
        return p;
    }
    return (p & 0xFF00FF00u) | ((p >> 16) & 0x000000FFu) | ((p & 0x000000FFu) << 16);
}

// One scanline. `src` and `dst` must not overlap partially (identical pointers
// are fine — the transform is per-pixel and in-place safe).
inline void rlottieRowToAndroid(const std::uint32_t* src, std::uint32_t* dst,
                                std::size_t pixels) {
    for (std::size_t x = 0; x < pixels; ++x) {
        dst[x] = rlottiePixelToAndroid(src[x]);
    }
}

// Whole surface. `srcStrideBytes`/`dstStrideBytes` are scanline strides (the
// Bitmap's stride is often wider than width*4). Both strides must be 4-byte
// aligned, which AndroidBitmap_getInfo guarantees for ARGB_8888.
inline void rlottieSurfaceToAndroid(const void* src, std::size_t srcStrideBytes, void* dst,
                                    std::size_t dstStrideBytes, std::size_t width,
                                    std::size_t height) {
    const auto* srcBytes = static_cast<const std::uint8_t*>(src);
    auto* dstBytes = static_cast<std::uint8_t*>(dst);
    for (std::size_t y = 0; y < height; ++y) {
        rlottieRowToAndroid(reinterpret_cast<const std::uint32_t*>(srcBytes + y * srcStrideBytes),
                            reinterpret_cast<std::uint32_t*>(dstBytes + y * dstStrideBytes), width);
    }
}

}  // namespace rnrlottie
