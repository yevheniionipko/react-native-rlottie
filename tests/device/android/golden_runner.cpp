// Chunk 7.4 (Part C) — the Android golden runner that actually executes on a
// booted emulator/device, as a plain native executable (no JNI, no APK, no
// gradle — see scripts/run-golden-android.sh for how it gets built/pushed/run).
//
// This intentionally goes through RlottiePlayerCore (the same shared core the
// JNI layer drives), not a bare rlottie::Animation — the whole point of a
// device leg is to prove the real, shipped code path renders identically on
// an actual Android arm64 process, not just on a macOS host. It re-uses
// Part A's ONE fixture table (GoldenFixtures.h) and ONE comparator
// (GoldenCompare.h) rather than a hand-copied subset, for the same
// drift-hazard reason those headers exist. Do not add a fixture list here.
//
// Three legs live in this one binary:
//
//   LEG 1 (core, byte-exact): loadFromData + renderFrame every golden frame in
//   the table, compare byte-exact against the committed
//   tests/golden/<name>-frame<N>.argb32.raw via GoldenCompare's
//   compareSurfaces() with a zero tolerance — identical in spirit to
//   tests/cpp/golden_tests.cpp's golden_frames_are_byte_exact, just running on
//   the emulator's real ABI/libc instead of the host.
//
//   LEG 2 (conversion): for every golden frame, run the committed golden bytes
//   through the shipped rlottieSurfaceToAndroid() (AndroidPixelConvert.h, the
//   exact function nativeCopyFrontInto uses) into a destination whose stride
//   is WIDER than width*4 — the way a real Bitmap's row stride usually is —
//   and assert byte-for-byte that R,G,B,A land where they should (mirroring
//   tests/cpp/android_tests.cpp's jni_golden_to_bitmap_bytes), that the
//   padding past width*4 is never written, and that swapping back is lossless.
//   It also writes a TIGHTLY-PACKED (stride == width*4) RGBA copy of every
//   frame plus a manifest to an output directory, which is the only thing
//   LEG 3 (BitmapGoldenMain.java, run separately under app_process) consumes.
//   Deliberately no fixture-name list is duplicated into the .java file —
//   the manifest is the sole source Leg 3 reads, generated fresh by this run.
//
// Env vars (both read by GoldenFixtures.h / this file, not by the shell
// script's push step):
//   RNRLOTTIE_GOLDEN_DIR  fixture/golden data root — the emulator's filesystem
//                         layout is unrelated to the host repo, so this is
//                         mandatory on device (GoldenFixtures.h falls back to
//                         a compiled-in host path otherwise, which is
//                         meaningless here).
//
// Usage: golden_runner <output-dir-for-leg2-conversion-artifacts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "AndroidPixelConvert.h"
#include "GoldenCompare.h"
#include "GoldenFixtures.h"
#include "RlottiePlayerCore.h"

using rnrlottie::RlottiePlayerCore;
namespace golden = rnrlottie::golden;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL: %s\n", what.c_str());
    }
}

bool readTextFile(const std::string& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

bool writeRawFile(const std::string& path, const std::uint8_t* data, std::size_t bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    return f.good();
}

// --- LEG 1 -------------------------------------------------------------------

bool runCoreLeg(const golden::Fixture& fx, std::size_t frame, std::vector<std::uint8_t>* golden) {
    std::string json;
    const std::string jsonPath = golden::fixtureJsonPath(fx);
    if (!readTextFile(jsonPath, &json)) {
        check(false, std::string(fx.name) + ": cannot read fixture json at " + jsonPath +
                         " (did the push step run? is RNRLOTTIE_GOLDEN_DIR set?)");
        return false;
    }

    RlottiePlayerCore core;
    auto load = core.loadFromData(json, std::string("android-golden-") + fx.name, "", false);
    check(load.success, std::string(fx.name) + ": loadFromData failed: " + load.error.message);
    if (!load.success) return false;

    std::vector<std::uint32_t> rendered(fx.width * fx.height, 0);
    const bool rendered_ok =
        core.renderFrame(frame, rendered.data(), fx.width, fx.height, fx.width * 4, true);
    check(rendered_ok,
          std::string(fx.name) + " frame " + std::to_string(frame) + ": renderFrame failed");
    if (!rendered_ok) return false;

    const std::string goldenPath = golden::goldenFramePath(fx.name, frame);
    if (!golden::fileExists(goldenPath)) {
        check(false, std::string(fx.name) + " frame " + std::to_string(frame) +
                          ": golden .raw missing at " + goldenPath + " (push step?)");
        return false;
    }
    const auto goldenBytes = golden::readRawFile(goldenPath);
    const std::size_t expectedBytes = rendered.size() * sizeof(std::uint32_t);
    check(goldenBytes.size() == expectedBytes,
          std::string(fx.name) + " frame " + std::to_string(frame) + ": golden size mismatch");
    if (goldenBytes.size() != expectedBytes) return false;

    const auto* renderedBytes = reinterpret_cast<const std::uint8_t*>(rendered.data());
    const std::size_t stride = fx.width * 4;
    const auto cmp = golden::compareSurfaces(renderedBytes, stride, goldenBytes.data(), stride,
                                              fx.width, fx.height, golden::ToleranceOptions{});
    if (!cmp.pass) {
        std::fprintf(stderr,
                      "  %s frame %zu: %zu differing pixel(s), worst delta=%d, first at (%zu,%zu)\n",
                      fx.name, frame, cmp.differingPixels, cmp.worstDelta.worst(), cmp.firstDiffX,
                      cmp.firstDiffY);
    }
    check(cmp.pass, std::string(fx.name) + " frame " + std::to_string(frame) +
                         ": on-device render != committed golden (byte-exact)");

    if (golden) *golden = goldenBytes;
    return cmp.pass;
}

// --- LEG 2 -------------------------------------------------------------------
//
// Runs the committed golden bytes (not the freshly-rendered ones — Leg 1
// already proved those match) through the exact conversion function the JNI
// blit calls, into a padded-stride destination the way a real Bitmap's row
// stride usually exceeds width*4, then:
//   (a) asserts every destination byte landed at the expected R/G/B/A offset,
//   (b) asserts stride padding was never written,
//   (c) round-trips back and asserts it's bit-identical to the source.
// Also writes a tightly-packed RGBA copy for Leg 3 and appends a manifest
// line so BitmapGoldenMain.java never has to hardcode a fixture list.
void runConversionLeg(const golden::Fixture& fx, std::size_t frame,
                       const std::vector<std::uint8_t>& src, const std::string& outDir,
                       std::ofstream& manifest) {
    using rnrlottie::kByteOffsetAlpha;
    using rnrlottie::kByteOffsetBlue;
    using rnrlottie::kByteOffsetGreen;
    using rnrlottie::kByteOffsetRed;
    using rnrlottie::rlottieSurfaceToAndroid;

    const std::size_t w = fx.width, h = fx.height;
    const std::size_t srcStride = w * 4;
    const std::size_t dstStride = (w + 7) * 4;  // padded, like a real Bitmap's row stride

    std::vector<std::uint8_t> padded(dstStride * h, 0xCD);
    rlottieSurfaceToAndroid(src.data(), srcStride, padded.data(), dstStride, w, h);

    // (a) explicit per-pixel channel-order check (not just "round trip is an
    // involution", which would pass even if the swap were a no-op on both
    // legs — this is the direct measurement docs/pixel-format-report.md's R<->B
    // claim needs).
    bool orderOk = true;
    for (std::size_t y = 0; orderOk && y < h; ++y) {
        const std::uint8_t* s = src.data() + y * srcStride;      // rlottie: B,G,R,A
        const std::uint8_t* d = padded.data() + y * dstStride;   // Android: R,G,B,A
        for (std::size_t x = 0; x < w; ++x) {
            const std::uint8_t* sp = s + x * 4;
            const std::uint8_t* dp = d + x * 4;
            if (dp[0] != sp[kByteOffsetRed] || dp[1] != sp[kByteOffsetGreen] ||
                dp[2] != sp[kByteOffsetBlue] || dp[3] != sp[kByteOffsetAlpha]) {
                orderOk = false;
                break;
            }
        }
    }
    check(orderOk, std::string(fx.name) + " frame " + std::to_string(frame) +
                        ": converted bytes are not in R,G,B,A order at every pixel");

    // (b) padding untouched.
    bool paddingOk = true;
    for (std::size_t y = 0; paddingOk && y < h; ++y) {
        for (std::size_t b = w * 4; b < dstStride; ++b) {
            if (padded[y * dstStride + b] != 0xCD) {
                paddingOk = false;
                break;
            }
        }
    }
    check(paddingOk, std::string(fx.name) + " frame " + std::to_string(frame) +
                          ": conversion wrote past width*4 into Bitmap row padding");

    // (c) round trip (involution) is lossless.
    std::vector<std::uint8_t> roundTripped(srcStride * h, 0);
    rlottieSurfaceToAndroid(padded.data(), dstStride, roundTripped.data(), srcStride, w, h);
    const bool roundTripOk =
        roundTripped.size() == src.size() &&
        std::memcmp(roundTripped.data(), src.data(), src.size()) == 0;
    check(roundTripOk, std::string(fx.name) + " frame " + std::to_string(frame) +
                            ": swap-back round trip is not lossless");

    // Tightly-packed RGBA (stride == width*4) for Leg 3 — a real Bitmap
    // created purely in Java (Bitmap.createBitmap(w,h,ARGB_8888), no hardware
    // buffer involved) has this exact row layout.
    std::vector<std::uint8_t> tight(w * h * 4);
    rlottieSurfaceToAndroid(src.data(), srcStride, tight.data(), w * 4, w, h);

    const std::string fileName =
        std::string(fx.name) + "-frame" + std::to_string(frame) + ".rgba";
    const std::string filePath = outDir + "/" + fileName;
    const bool wrote = writeRawFile(filePath, tight.data(), tight.size());
    check(wrote, std::string(fx.name) + " frame " + std::to_string(frame) +
                     ": failed to write conversion artifact " + filePath);
    if (wrote) {
        manifest << fx.name << ' ' << frame << ' ' << w << ' ' << h << ' ' << fileName << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string outDir = argc > 1 ? argv[1] : ".";

    std::printf("=== Chunk 7.4 (Part C) Android device golden runner ===\n");
    std::printf("data root (RNRLOTTIE_GOLDEN_DIR or fallback): %s\n", golden::dataRoot().c_str());
    std::printf("leg-2/3 output dir: %s\n\n", outDir.c_str());

    const std::string manifestPath = outDir + "/manifest.txt";
    std::ofstream manifest(manifestPath, std::ios::binary | std::ios::trunc);
    if (!manifest) {
        std::fprintf(stderr, "cannot open manifest for write at %s (does outDir exist?)\n",
                     manifestPath.c_str());
        return 2;
    }

    for (const auto& fx : golden::allFixtures()) {
        std::printf("-- fixture: %s --\n", fx.name);
        for (std::size_t frame : fx.goldenFrames) {
            std::vector<std::uint8_t> goldenBytes;
            const bool coreOk = runCoreLeg(fx, frame, &goldenBytes);
            if (coreOk) {
                runConversionLeg(fx, frame, goldenBytes, outDir, manifest);
            } else {
                std::fprintf(stderr,
                              "  skipping conversion leg for %s frame %zu (core leg failed)\n",
                              fx.name, frame);
            }
        }
    }
    manifest.close();

    std::printf("\n=== %d/%d checks passed ===\n", g_checks - g_failures, g_checks);
    if (g_failures == 0) {
        std::printf("CORE + CONVERSION LEGS: PASS\n");
        return 0;
    }
    std::fprintf(stderr, "CORE + CONVERSION LEGS: FAIL (%d failing check(s))\n", g_failures);
    return 1;
}
