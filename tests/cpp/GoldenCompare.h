// Chunk 7.4 (Part A) — the ONE comparator shared by the host gate and the
// forthcoming iOS-simulator / Android-emulator golden runners.
//
// Header-only, dependency-free (std library only, no rlottie, no test_harness)
// so any of the three runners can #include it standalone, exactly as
// GoldenFixtures.h and AndroidPixelConvert.h already are (Chunk 3.1's
// precedent: JNI-free headers keep host tests and the real Android build on
// the same code path).
//
// Two comparison legs, on purpose:
//
//   1. THE CORE LEG is byte-exact (tolerance 0/0). rlottie is the same C++
//      binary on iOS and Android — no platform code sits between "core
//      renders a frame" and "here is the ARGB32 buffer". If that leg needed
//      tolerance, it would mean the render itself is nondeterministic across
//      runs/platforms, which is a correctness bug to fix, not a golden to
//      loosen.
//   2. THE PLATFORM-CONVERSION LEG (Part B/C device runners) gets tolerance.
//      A device runner reads the frame back out of a real UIImage/CGImage or
//      a real android.graphics.Bitmap — color-management, GPU compositing,
//      and lossy readback paths (e.g. a screenshot API) can perturb channel
//      values by a few 8-bit steps even though the pixels are "the same
//      picture". `ToleranceOptions` bounds exactly how much: a small
//      per-channel delta AND a small fraction of pixels allowed to differ at
//      all, so a systematic corruption (wrong channel order, a stuck row)
//      still fails loudly instead of hiding under a generous global average.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace rnrlottie::golden {

// Per-channel worst-case absolute delta observed across the whole compare.
// Channel names follow rlottie's own memory byte order (PixelFormat.h):
// byte[0]=Blue, byte[1]=Green, byte[2]=Red, byte[3]=Alpha.
struct ChannelDelta {
    int blue = 0;
    int green = 0;
    int red = 0;
    int alpha = 0;

    int worst() const { return std::max(std::max(blue, green), std::max(red, alpha)); }
};

struct CompareResult {
    bool pass = false;
    std::size_t totalPixels = 0;
    std::size_t differingPixels = 0;
    ChannelDelta worstDelta;
    // Only meaningful when differingPixels > 0.
    bool hasDiff = false;
    std::size_t firstDiffX = 0;
    std::size_t firstDiffY = 0;

    double differingFraction() const {
        return totalPixels == 0 ? 0.0 : double(differingPixels) / double(totalPixels);
    }
};

// tolerance == {0, 0.0} is byte-exact comparison — the CORE leg above.
struct ToleranceOptions {
    int maxPerChannelDelta = 0;
    double maxDifferingFraction = 0.0;
};

// Compares two ARGB32 (B,G,R,A) surfaces of identical width/height. Strides
// are scanline byte strides (may exceed width*4 — a real Bitmap's usually
// does); both must be 4-byte aligned, which every caller here already
// guarantees (rlottie's own stride contract / AndroidBitmap_getInfo).
inline CompareResult compareSurfaces(const std::uint8_t* a, std::size_t aStrideBytes,
                                      const std::uint8_t* b, std::size_t bStrideBytes,
                                      std::size_t width, std::size_t height,
                                      const ToleranceOptions& tol) {
    CompareResult res;
    res.totalPixels = width * height;

    for (std::size_t y = 0; y < height; ++y) {
        const std::uint8_t* rowA = a + y * aStrideBytes;
        const std::uint8_t* rowB = b + y * bStrideBytes;
        for (std::size_t x = 0; x < width; ++x) {
            const std::uint8_t* pa = rowA + x * 4;
            const std::uint8_t* pb = rowB + x * 4;
            const int db = std::abs(int(pa[0]) - int(pb[0]));
            const int dg = std::abs(int(pa[1]) - int(pb[1]));
            const int dr = std::abs(int(pa[2]) - int(pb[2]));
            const int da = std::abs(int(pa[3]) - int(pb[3]));

            if (db != 0 || dg != 0 || dr != 0 || da != 0) {
                if (!res.hasDiff) {
                    res.hasDiff = true;
                    res.firstDiffX = x;
                    res.firstDiffY = y;
                }
                ++res.differingPixels;
            }
            res.worstDelta.blue = std::max(res.worstDelta.blue, db);
            res.worstDelta.green = std::max(res.worstDelta.green, dg);
            res.worstDelta.red = std::max(res.worstDelta.red, dr);
            res.worstDelta.alpha = std::max(res.worstDelta.alpha, da);
        }
    }

    // The worst per-channel delta bounds EVERY pixel's delta by construction
    // (it is a max over all of them), so comparing it once here is equivalent
    // to — and cheaper than — re-checking each differing pixel individually.
    res.pass = res.worstDelta.worst() <= tol.maxPerChannelDelta &&
               res.differingFraction() <= tol.maxDifferingFraction;
    return res;
}

// --- Non-vacuity checks (Part A requirement 2) ------------------------------
//
// A syntactically-valid Lottie that rlottie happens to render as fully
// transparent (or as one flat, uninteresting color) would make the golden
// gate pass vacuously — "matches golden" is trivially true for an empty
// buffer compared to another empty buffer. Both host and device runners MUST
// use these same two numbers so a fixture can't quietly rot into vacuity on
// one platform while still "passing" on another.
struct NonVacuityStats {
    std::size_t totalPixels = 0;
    std::size_t nonTransparentPixels = 0;
    std::size_t distinctColors = 0;

    double nonTransparentFraction() const {
        return totalPixels == 0 ? 0.0 : double(nonTransparentPixels) / double(totalPixels);
    }
};

inline NonVacuityStats computeNonVacuity(const std::uint8_t* buf, std::size_t strideBytes,
                                          std::size_t width, std::size_t height) {
    NonVacuityStats stats;
    stats.totalPixels = width * height;
    std::unordered_set<std::uint32_t> colors;
    colors.reserve(width * height);

    for (std::size_t y = 0; y < height; ++y) {
        const std::uint8_t* row = buf + y * strideBytes;
        for (std::size_t x = 0; x < width; ++x) {
            const std::uint8_t* p = row + x * 4;
            const std::uint32_t word = (std::uint32_t(p[0])) | (std::uint32_t(p[1]) << 8) |
                                        (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
            colors.insert(word);
            if (p[3] != 0) ++stats.nonTransparentPixels;
        }
    }
    stats.distinctColors = colors.size();
    return stats;
}

// Reads a whole file into bytes. Returns an empty vector on failure — callers
// distinguish "empty file" from "missing file" via exists() below, since an
// empty-but-present golden would itself be a non-vacuity bug worth catching
// separately rather than silently masking as "file not found".
inline bool fileExists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

inline std::vector<std::uint8_t> readRawFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

}  // namespace rnrlottie::golden
