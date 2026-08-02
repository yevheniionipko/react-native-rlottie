// Authoritative pixel-format facts for rlottie's render surface.
// Determined empirically in Chunk 0.4 — see docs/pixel-format-report.md.
//
// rlottie renders into a caller-provided buffer as ARGB32 PREMULTIPLIED with
// memory byte order B,G,R,A (little-endian 32-bit word 0xAARRGGBB), top-down
// rows, stride = the caller's bytesPerLine. This is platform-independent; only
// the platform presenters reinterpret the bytes.
#pragma once

#include <cstddef>

namespace rnrlottie {

// Bytes per pixel in the rlottie surface (ARGB32).
inline constexpr std::size_t kBytesPerPixel = 4;

// Memory byte offsets within one pixel (little-endian ARGB32 = word 0xAARRGGBB).
inline constexpr int kByteOffsetBlue = 0;
inline constexpr int kByteOffsetGreen = 1;
inline constexpr int kByteOffsetRed = 2;
inline constexpr int kByteOffsetAlpha = 3;

// Alpha is premultiplied into the color channels.
inline constexpr bool kSurfaceIsPremultiplied = true;

// Row 0 is the top of the image; presenters need no vertical flip.
inline constexpr bool kSurfaceIsTopDown = true;

// Android Bitmap.Config.ARGB_8888 is R,G,B,A in native memory, so the red and
// blue channels must be swapped relative to rlottie's B,G,R,A. Premultiplication
// already matches (ARGB_8888 is premultiplied), so no alpha rework is needed.
// iOS Core Graphics needs NO swap with
// (kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little).
inline constexpr bool kAndroidNeedsChannelSwap = true;

}  // namespace rnrlottie
