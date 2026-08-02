// Chunk 7.4 (Part B) — the iOS-simulator golden runner that ACTUALLY EXECUTES
// on a booted Simulator (unlike the host `tests/cpp` suite, which proves the
// core is correct on the *development machine's* clang/arm64, not on the
// iOS runtime path a real app takes).
//
// Reads the ONE fixture table (tests/cpp/GoldenFixtures.h) and uses the ONE
// comparator (tests/cpp/GoldenCompare.h) the host gate already uses — see
// those headers' own comments for why a per-platform copy is a drift hazard
// this chunk exists to avoid. This file and scripts/run-golden-ios.sh are the
// ONLY new files Part B owns; GoldenFixtures.h/GoldenCompare.h/the host
// golden_*.cpp files belong to Part A and are read-only from here.
//
// Two legs per fixture, per golden frame (see GoldenCompare.h's header for the
// rationale for why they get different tolerances):
//
//   LEG 1 — CORE, byte-exact. rnrlottie::RlottiePlayerCore renders straight
//   into a tightly-packed buffer, exactly as golden_tests.cpp does on the
//   host, and the result is compared byte-for-byte against the SAME committed
//   tests/golden/*.raw. This is what actually answers "is rlottie's output
//   identical on the simulator to the host golden" — nothing about the CPU
//   architecture, libc++, or Foundation runtime should be able to perturb it,
//   and if it did, that would be a real (and serious) rlottie determinism bug.
//
//   LEG 2 — PLATFORM CONVERSION, tolerance-bounded. The REAL shipped
//   ios/RNRlottieFramePresenter.mm (compiled directly into this binary, not
//   reimplemented here — the point is testing the code the app ships, per
//   the task) turns the FrameBuffer front buffer into a CGImageRef, which is
//   then drawn into a CGBitmapContext using the exact documented format
//   (docs/pixel-format-report.md §4: kCGImageAlphaPremultipliedFirst |
//   kCGBitmapByteOrder32Little, DeviceRGB, top-down, no channel swap) and
//   read back for comparison. This is the ONLY leg that can catch a real
//   CoreGraphics-side surprise (color management, a stride bug in
//   CGImageCreate's bytesPerRow handling, alpha handling) since LEG 1 never
//   goes anywhere near CoreGraphics.
//
//   LEG 2 runs TWICE per frame: once over the FrameBuffer's own real front
//   buffer (which cpp/FrameBuffer.cpp always allocates tightly packed —
//   bytesPerLine() is literally `width * 4`, see FrameBuffer::bytesPerLine;
//   grep confirms no code path in this codebase ever hands it a padded
//   stride), and once more over a SYNTHETIC FrameBuffer::FrontBuffer this
//   runner builds by hand: same pixel content, copied row-by-row into a
//   buffer whose stride is deliberately wider than width*4, with the padding
//   bytes poisoned (0xCD) so an out-of-bounds read/write would be visible.
//   FrontBuffer (cpp/FrameBuffer.h) is a plain struct of public fields with no
//   invariant besides "pixels/width/height/bytesPerLine describe a real
//   surface", so handing the presenter a hand-built one exercises exactly the
//   same `newImageForFrontBuffer:` code path a padded real-world source
//   (e.g. a locked Bitmap, or a future FrameBuffer variant) would hit — this
//   is the case the task calls out as "exactly a stride bug is what this leg
//   exists to catch", and FrameBuffer itself cannot produce it today, so a
//   synthetic one is the only way to cover it.
//
// Tolerance starts at 0/0 (byte-exact) for LEG 2, per instructions — only
// loosen it if a real, explained CoreGraphics difference shows up.
//
// RESULT (verified on the booted iPhone 16 Pro / iOS 18.2 simulator,
// UDID CF997755-9F37-48BC-BD02-97D9FF30D25E, arm64 host): every fixture, every
// golden frame, both legs (including the padded-stride variant and the
// padding-poison check) are BYTE-EXACT. Tolerance was NOT loosened — it stays
// at 0/0. One real bug was found and fixed, in this HARNESS, not in the
// shipped presenter: the first attempt drew each CGImage into the readback
// CGBitmapContext with CoreGraphics' default blend mode (source-over), so a
// transparent source pixel left the pre-draw poison byte (0xEE) showing
// through underneath it instead of overwriting it with (0,0,0,0) — every
// fixture with any transparent content (masks, transparency, markers, and
// parts of gradients/precomp/shapes) then "failed" at ~70-100% differing
// pixels purely from that harness artifact. Explicitly setting
// CGContextSetBlendMode(ctx, kCGBlendModeCopy) (see readBackViaCoreGraphics
// below) makes the readback reflect the CGImage's literal pixel data, which
// is what a byte-exact comparison actually needs; after that fix every check
// passed with zero differing pixels.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "FrameBuffer.h"
#include "GoldenCompare.h"
#include "GoldenFixtures.h"
#include "RlottiePlayerCore.h"

#import "RNRlottieFramePresenter.h"
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

using rnrlottie::FrameBuffer;
using rnrlottie::RlottiePlayerCore;
namespace golden = rnrlottie::golden;

namespace {

int gFailures = 0;

std::string readTextFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "golden_runner: cannot open %s\n", path.c_str());
        std::exit(2);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Prints a CompareResult in the same shape golden_tests.cpp uses (fixture,
// frame, leg label) plus the full per-channel worst delta (not just the max
// across channels) so a systematic single-channel bug — e.g. only Red is
// off, which a swapped-channel presenter would produce — is diagnosable from
// the log alone.
void report(const char* fixture, std::size_t frame, const char* leg,
            const golden::CompareResult& cmp, const golden::ToleranceOptions& tol) {
    const char* verdict = cmp.pass ? "PASS" : "FAIL";
    std::printf("  [%s] %s frame %-3zu %-28s differing=%zu/%zu (%.4f%%)  worst(B,G,R,A)=(%d,%d,%d,%d)"
                "  tol(delta<=%d, frac<=%.4f)\n",
                verdict, fixture, frame, leg, cmp.differingPixels, cmp.totalPixels,
                100.0 * cmp.differingFraction(), cmp.worstDelta.blue, cmp.worstDelta.green,
                cmp.worstDelta.red, cmp.worstDelta.alpha, tol.maxPerChannelDelta,
                tol.maxDifferingFraction);
    if (!cmp.pass) {
        if (cmp.hasDiff) {
            std::printf("         first differing pixel at (%zu,%zu)\n", cmp.firstDiffX,
                        cmp.firstDiffY);
        }
        ++gFailures;
    }
}

// --- LEG 1: core, byte-exact -------------------------------------------------

void runCoreLeg(const golden::Fixture& fx, std::size_t frame, RlottiePlayerCore& core) {
    std::vector<std::uint32_t> rendered(fx.width * fx.height, 0);
    const std::size_t stride = fx.width * 4;
    if (!core.renderFrame(frame, rendered.data(), fx.width, fx.height, stride, true)) {
        std::fprintf(stderr, "  [FAIL] %s frame %zu: core leg renderFrame() failed\n", fx.name,
                     frame);
        ++gFailures;
        return;
    }

    const std::string goldenPath = golden::goldenFramePath(fx.name, frame);
    if (!golden::fileExists(goldenPath)) {
        std::fprintf(stderr, "  [FAIL] %s frame %zu: missing golden %s\n", fx.name, frame,
                     goldenPath.c_str());
        ++gFailures;
        return;
    }
    const auto goldenBytes = golden::readRawFile(goldenPath);
    if (goldenBytes.size() != rendered.size() * sizeof(std::uint32_t)) {
        std::fprintf(stderr, "  [FAIL] %s frame %zu: golden size mismatch (%zu vs %zu)\n", fx.name,
                     frame, goldenBytes.size(), rendered.size() * sizeof(std::uint32_t));
        ++gFailures;
        return;
    }

    const auto* renderedBytes = reinterpret_cast<const std::uint8_t*>(rendered.data());
    const golden::ToleranceOptions tol{};  // byte-exact
    const auto cmp =
        golden::compareSurfaces(renderedBytes, stride, goldenBytes.data(), stride, fx.width,
                                fx.height, tol);
    report(fx.name, frame, "core (byte-exact)", cmp, tol);
}

// --- LEG 2: platform conversion via the REAL RNRlottieFramePresenter --------

// Draws `image` (expected to be exactly width x height, no scaling) into a
// freshly allocated CGBitmapContext using the documented iOS format
// (docs/pixel-format-report.md §4), then hands back the raw pixel bytes for
// comparison. Returns an empty vector on any CoreGraphics failure.
std::vector<std::uint8_t> readBackViaCoreGraphics(CGImageRef image, std::size_t width,
                                                   std::size_t height) {
    std::vector<std::uint8_t> pixels(width * 4 * height, 0xEE);  // poison before draw
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    const CGBitmapInfo bitmapInfo =
        (CGBitmapInfo)kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaPremultipliedFirst;
    CGContextRef ctx = CGBitmapContextCreate(pixels.data(), width, height, 8, width * 4,
                                             colorSpace, bitmapInfo);
    CGColorSpaceRelease(colorSpace);
    if (!ctx) {
        return {};
    }
    // kCGBlendModeCopy, not the default source-over: this is a raw pixel
    // READBACK, not a compositing operation, so a transparent source pixel
    // must overwrite the destination with (0,0,0,0) rather than leave the
    // poison byte (0xEE) showing through underneath it. Source-over was
    // tried first and produced spurious ~100%-differing-pixel "failures" on
    // every fixture with any transparency (masks, transparency, markers) —
    // that was a bug in THIS harness (comparing rlottie's real alpha=0
    // pixels against a blended-with-poison background), not in the shipped
    // presenter; kCGBlendModeCopy makes the readback reflect the CGImage's
    // literal pixel data.
    CGContextSetBlendMode(ctx, kCGBlendModeCopy);
    // No scaling: draw at exactly the image's own pixel size so interpolation
    // (the presenter sets shouldInterpolate=true, meant for resizing views,
    // not this readback) cannot blur a single sample and hide a real delta.
    CGContextDrawImage(ctx, CGRectMake(0, 0, width, height), image);
    CGContextRelease(ctx);
    return pixels;
}

// Asserts the contract points from docs/pixel-format-report.md §4 that only a
// real CGImageRef can confirm (a host-side unit test has no CoreGraphics to
// ask): the image was built with premultiplied-first alpha and little-endian
// byte order (i.e. no channel swap was applied — matching "no channel swap
// needed on iOS"), 8 bits/component, 32 bits/pixel.
void assertPresenterContract(const char* fixture, std::size_t frame, CGImageRef image) {
    const CGImageAlphaInfo alpha = CGImageGetAlphaInfo(image);
    const CGBitmapInfo bitmapInfo = CGImageGetBitmapInfo(image);
    const CGBitmapInfo byteOrder = bitmapInfo & kCGBitmapByteOrderMask;
    const std::size_t bpc = CGImageGetBitsPerComponent(image);
    const std::size_t bpp = CGImageGetBitsPerPixel(image);

    bool ok = true;
    if (alpha != kCGImageAlphaPremultipliedFirst) {
        std::fprintf(stderr,
                     "  [FAIL] %s frame %zu: presenter alphaInfo=%d, expected "
                     "kCGImageAlphaPremultipliedFirst (%d) — premultiplied-alpha contract broken\n",
                     fixture, frame, (int)alpha, (int)kCGImageAlphaPremultipliedFirst);
        ok = false;
    }
    if (byteOrder != (CGBitmapInfo)kCGBitmapByteOrder32Little) {
        std::fprintf(stderr,
                     "  [FAIL] %s frame %zu: presenter byteOrder=%d, expected "
                     "kCGBitmapByteOrder32Little (%d) — this is the flag that makes memory "
                     "order B,G,R,A need NO channel swap on iOS; a wrong value here means the "
                     "\"no swap\" claim in RNRlottieFramePresenter.mm's header comment is false\n",
                     fixture, frame, (int)byteOrder, (int)kCGBitmapByteOrder32Little);
        ok = false;
    }
    if (bpc != 8 || bpp != 32) {
        std::fprintf(stderr, "  [FAIL] %s frame %zu: bpc=%zu bpp=%zu, expected 8/32\n", fixture,
                     frame, bpc, bpp);
        ok = false;
    }
    if (ok) {
        std::printf("  [PASS] %s frame %-3zu presenter contract (premultipliedFirst, "
                    "byteOrder32Little, 8bpc/32bpp — no channel swap on iOS)\n",
                    fixture, frame);
    } else {
        ++gFailures;
    }
}

// Runs LEG 2 once for a given FrontBuffer (either the FrameBuffer's own real
// one, or a synthetic padded-stride one — see the header comment). `label`
// distinguishes the two in the log.
void runPlatformLegOnce(const golden::Fixture& fx, std::size_t frame,
                        const rnrlottie::FrameBuffer::FrontBuffer& front,
                        RNRlottieFramePresenter* presenter, const char* label,
                        const golden::ToleranceOptions& tol) {
    CGImageRef image = [presenter newImageForFrontBuffer:front];
    if (image == nullptr) {
        std::fprintf(stderr, "  [FAIL] %s frame %zu: %s presenter returned NULL CGImageRef\n",
                     fx.name, frame, label);
        ++gFailures;
        return;
    }
    if (CGImageGetWidth(image) != front.width || CGImageGetHeight(image) != front.height) {
        std::fprintf(stderr, "  [FAIL] %s frame %zu: %s CGImage size %zux%zu != FrontBuffer %zux%zu\n",
                     fx.name, frame, label, (size_t)CGImageGetWidth(image),
                     (size_t)CGImageGetHeight(image), front.width, front.height);
        ++gFailures;
        CGImageRelease(image);
        return;
    }

    assertPresenterContract(fx.name, frame, image);

    const std::vector<std::uint8_t> readback = readBackViaCoreGraphics(image, front.width, front.height);
    CGImageRelease(image);
    if (readback.empty()) {
        std::fprintf(stderr, "  [FAIL] %s frame %zu: %s CGBitmapContextCreate failed\n", fx.name,
                     frame, label);
        ++gFailures;
        return;
    }

    const std::string goldenPath = golden::goldenFramePath(fx.name, frame);
    const auto goldenBytes = golden::readRawFile(goldenPath);
    if (goldenBytes.size() != front.width * front.height * 4) {
        std::fprintf(stderr, "  [FAIL] %s frame %zu: golden size mismatch for %s leg\n", fx.name,
                     frame, label);
        ++gFailures;
        return;
    }

    const std::size_t tightStride = front.width * 4;
    const auto cmp = golden::compareSurfaces(readback.data(), tightStride, goldenBytes.data(),
                                              tightStride, front.width, front.height, tol);
    std::string legLabel = std::string("platform (") + label + ")";
    report(fx.name, frame, legLabel.c_str(), cmp, tol);
}

void runPlatformLeg(const golden::Fixture& fx, std::size_t frame, RlottiePlayerCore& core,
                    RNRlottieFramePresenter* presenter, const golden::ToleranceOptions& tol) {
    // --- Real FrameBuffer, tight stride (this is what a normal render tick
    // actually hands the presenter today) ---
    FrameBuffer fb(FrameBuffer::Limits{});
    rnrlottie::PlayerError err;
    if (!fb.resize(FrameBuffer::Dimensions{fx.width, fx.height}, err)) {
        std::fprintf(stderr, "  [FAIL] %s frame %zu: FrameBuffer::resize failed (%s)\n", fx.name,
                     frame, err.message.c_str());
        ++gFailures;
        return;
    }
    std::uint32_t* target = fb.acquireRenderTarget();
    if (target == nullptr ||
        !core.renderFrame(frame, target, fx.width, fx.height, fb.bytesPerLine(), true)) {
        std::fprintf(stderr, "  [FAIL] %s frame %zu: platform-leg renderFrame() failed\n", fx.name,
                     frame);
        ++gFailures;
        return;
    }
    fb.publish();
    const rnrlottie::FrameBuffer::FrontBuffer realFront = fb.readFront();
    if (realFront.pixels == nullptr) {
        std::fprintf(stderr, "  [FAIL] %s frame %zu: readFront() returned null after publish()\n",
                     fx.name, frame);
        ++gFailures;
        return;
    }
    // FrameBuffer::bytesPerLine() is always width*4 (cpp/FrameBuffer.cpp) — no
    // code path in this codebase produces a padded real front buffer, so this
    // is a sanity check on that claim, not a tautology worth failing loudly
    // over: if it ever changes, the "padded" case below becomes redundant
    // rather than the only coverage, which is a fine direction to be wrong in.
    runPlatformLegOnce(fx, frame, realFront, presenter, "real, tight stride", tol);

    // --- Synthetic FrontBuffer, DELIBERATELY padded stride ---
    // Copies the same pixel content row-by-row into a wider-stride buffer,
    // poisoning (0xCD) the padding bytes so an out-of-bounds read by the
    // presenter (or by CGImageCreate given a bytesPerRow it mishandles) would
    // perturb the poison and be visible if we chose to check it (we assert
    // the visual comparison instead, which is the actually-shipping-bug
    // shape — a stride bug shows as a diagonal skew in the decoded image,
    // not necessarily a poison-byte read).
    constexpr std::size_t kPadPixels = 7;  // arbitrary, just > 0
    const std::size_t paddedStride = (fx.width + kPadPixels) * 4;
    std::vector<std::uint8_t> padded(paddedStride * fx.height, 0xCD);
    const auto* srcBytes = reinterpret_cast<const std::uint8_t*>(realFront.pixels);
    for (std::size_t y = 0; y < fx.height; ++y) {
        std::memcpy(padded.data() + y * paddedStride, srcBytes + y * realFront.bytesPerLine,
                    fx.width * 4);
    }
    rnrlottie::FrameBuffer::FrontBuffer paddedFront{
        reinterpret_cast<const std::uint32_t*>(padded.data()), fx.width, fx.height, paddedStride};
    runPlatformLegOnce(fx, frame, paddedFront, presenter, "synthetic, padded stride", tol);

    // Confirm the presenter never wrote into padding it doesn't own — it only
    // ever reads pixel memory (see RNRlottieFramePresenter.mm's ReleaseNoOp:
    // it does not own or free the buffer, and never mutates it), so the
    // poison bytes must be untouched after building+drawing the CGImage.
    for (std::size_t y = 0; y < fx.height; ++y) {
        for (std::size_t b = fx.width * 4; b < paddedStride; ++b) {
            if (padded[y * paddedStride + b] != 0xCD) {
                std::fprintf(stderr,
                             "  [FAIL] %s frame %zu: padding byte at row %zu col %zu was "
                             "overwritten (0x%02X) — presenter wrote past width*4\n",
                             fx.name, frame, y, b, padded[y * paddedStride + b]);
                ++gFailures;
            }
        }
    }
}

int runFixture(const golden::Fixture& fx, RNRlottieFramePresenter* presenter,
              const golden::ToleranceOptions& platformTol) {
    const std::string json = readTextFile(golden::fixtureJsonPath(fx));
    RlottiePlayerCore core;
    auto load = core.loadFromData(json, std::string("ios-golden-runner-") + fx.name, "", false);
    if (!load.success) {
        std::fprintf(stderr, "== %s: FAILED TO LOAD (code=%d): %s ==\n", fx.name,
                     (int)load.error.code, load.error.message.c_str());
        ++gFailures;
        return gFailures;
    }
    if (load.metadata.width != fx.width || load.metadata.height != fx.height) {
        std::fprintf(stderr, "== %s: metadata %zux%zu != table %zux%zu ==\n", fx.name,
                     load.metadata.width, load.metadata.height, fx.width, fx.height);
        ++gFailures;
    }

    std::printf("== %s (%zux%zu, %zu golden frame(s)) ==\n", fx.name, fx.width, fx.height,
               fx.goldenFrames.size());
    for (std::size_t frame : fx.goldenFrames) {
        runCoreLeg(fx, frame, core);
        runPlatformLeg(fx, frame, core, presenter, platformTol);
    }
    std::printf("\n");
    return gFailures;
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    @autoreleasepool {
        std::printf("=== Chunk 7.4 (Part B) iOS-simulator golden runner ===\n");
        std::printf("data root: %s\n\n", golden::dataRoot().c_str());

        // Byte-exact until proven otherwise — do not loosen speculatively.
        // See the bottom of this file (updated after the first real run) for
        // whether that held.
        const golden::ToleranceOptions platformTol{/*maxPerChannelDelta=*/0,
                                                    /*maxDifferingFraction=*/0.0};

        RNRlottieFramePresenter* presenter = [[RNRlottieFramePresenter alloc] init];
        for (const auto& fx : golden::allFixtures()) {
            runFixture(fx, presenter, platformTol);
        }
        presenter = nil;

        if (gFailures == 0) {
            std::printf("=== ALL GOLDEN CHECKS PASSED ON SIMULATOR ===\n");
            return 0;
        }
        std::fprintf(stderr, "=== %d GOLDEN CHECK FAILURE(S) — see above ===\n", gFailures);
        return 1;
    }
}
