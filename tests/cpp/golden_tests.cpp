// Chunk 7.4 (Part A) — the host half of cross-platform golden verification.
// Part of the DEFAULT gate (tests/run-tests.sh with no args): unlike
// golden_gen.cpp this has no main() of its own and is a normal TEST() file
// picked up by the fallback/CMake glob.
//
// Reads the ONE fixture table (GoldenFixtures.h) and the ONE comparator
// (GoldenCompare.h) that Parts B/C (an iOS-simulator runner and an
// Android-emulator native runner) will also #include, so a fixture or a
// tolerance rule can't drift between platforms.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "AndroidPixelConvert.h"
#include "GoldenCompare.h"
#include "GoldenFixtures.h"
#include "RlottiePlayerCore.h"
#include "test_harness.h"

using rnrlottie::RlottiePlayerCore;
namespace golden = rnrlottie::golden;

namespace {

std::string readJson(const golden::Fixture& fx) {
    return tst::readText(golden::fixtureJsonPath(fx));
}

// Renders `frame` from a freshly-loaded core into a tightly-packed
// (stride == width*4) buffer. Returns false (and leaves `out` untouched) on
// any load/render failure so callers can CHECK and continue rather than
// crash mid-suite.
bool renderGoldenFrame(const golden::Fixture& fx, std::size_t frame,
                       std::vector<std::uint32_t>& out) {
    RlottiePlayerCore core;
    auto load = core.loadFromData(readJson(fx), std::string("golden-test-") + fx.name, "", false);
    if (!load.success) return false;
    out.assign(fx.width * fx.height, 0);
    return core.renderFrame(frame, out.data(), fx.width, fx.height, fx.width * 4, true);
}

}  // namespace

// --- Metadata matches the table exactly (name/size/frames/duration/markers) -

TEST(golden_metadata_matches_table) {
    for (const auto& fx : golden::allFixtures()) {
        RlottiePlayerCore core;
        auto load = core.loadFromData(readJson(fx), std::string("golden-meta-") + fx.name, "",
                                      false);
        CHECK(load.success);
        if (!load.success) continue;

        CHECK(load.metadata.width == fx.width);
        CHECK(load.metadata.height == fx.height);
        CHECK(load.metadata.totalFrames == fx.totalFrames);
        CHECK(load.metadata.duration > fx.duration - 0.05 &&
              load.metadata.duration < fx.duration + 0.05);

        CHECK(load.metadata.markers.size() == fx.markers.size());
        const std::size_t n = std::min(load.metadata.markers.size(), fx.markers.size());
        for (std::size_t i = 0; i < n; ++i) {
            CHECK(load.metadata.markers[i].name == fx.markers[i].name);
            CHECK(load.metadata.markers[i].startFrame == fx.markers[i].startFrame);
            CHECK(load.metadata.markers[i].endFrame == fx.markers[i].endFrame);
        }
    }
}

// --- Every golden frame renders byte-exact against the committed .raw ------

TEST(golden_frames_are_byte_exact) {
    for (const auto& fx : golden::allFixtures()) {
        for (std::size_t frame : fx.goldenFrames) {
            std::vector<std::uint32_t> rendered;
            CHECK(renderGoldenFrame(fx, frame, rendered));
            if (rendered.empty()) continue;

            const std::string goldenPath = golden::goldenFramePath(fx.name, frame);
            CHECK(golden::fileExists(goldenPath));
            const auto goldenBytes = golden::readRawFile(goldenPath);
            CHECK(goldenBytes.size() == rendered.size() * sizeof(std::uint32_t));
            if (goldenBytes.size() != rendered.size() * sizeof(std::uint32_t)) continue;

            const auto* renderedBytes = reinterpret_cast<const std::uint8_t*>(rendered.data());
            const std::size_t stride = fx.width * 4;
            const auto cmp = golden::compareSurfaces(renderedBytes, stride, goldenBytes.data(),
                                                      stride, fx.width, fx.height,
                                                      golden::ToleranceOptions{});  // byte-exact
            if (!cmp.pass) {
                std::string msg = fx.name + std::string(" frame ") + std::to_string(frame) +
                                  ": " + std::to_string(cmp.differingPixels) +
                                  " differing pixel(s), worst delta=" +
                                  std::to_string(cmp.worstDelta.worst()) + " first at (" +
                                  std::to_string(cmp.firstDiffX) + "," +
                                  std::to_string(cmp.firstDiffY) + ")";
                std::printf("  %s\n", msg.c_str());
            }
            CHECK(cmp.pass);
        }
    }
}

// --- Non-vacuity: the committed goldens themselves are real content --------
//
// This checks the COMMITTED .raw files (not a fresh render) so it also
// catches "someone hand-edited/truncated a golden into blankness" — a
// mistake golden_frames_are_byte_exact alone would not distinguish from "the
// renderer legitimately changed" for an all-transparent regression.
TEST(golden_files_are_non_vacuous) {
    for (const auto& fx : golden::allFixtures()) {
        for (std::size_t frame : fx.goldenFrames) {
            const std::string goldenPath = golden::goldenFramePath(fx.name, frame);
            const auto bytes = golden::readRawFile(goldenPath);
            CHECK(bytes.size() == fx.width * fx.height * 4);
            if (bytes.size() != fx.width * fx.height * 4) continue;

            const auto stats = golden::computeNonVacuity(bytes.data(), fx.width * 4, fx.width,
                                                          fx.height);
            // Per-fixture floors from the table, not a global ">0" — see the
            // rationale on Fixture::minNonTransparentFraction.
            if (stats.nonTransparentFraction() < fx.minNonTransparentFraction ||
                stats.distinctColors < fx.minDistinctColors) {
                std::printf("  %s frame %zu: VACUITY FLOOR — nonTransparent=%.1f%% (min %.1f%%), "
                            "distinctColors=%zu (min %zu)\n",
                            fx.name, frame, 100.0 * stats.nonTransparentFraction(),
                            100.0 * fx.minNonTransparentFraction, stats.distinctColors,
                            fx.minDistinctColors);
            }
            CHECK(stats.nonTransparentFraction() >= fx.minNonTransparentFraction);
            CHECK(stats.distinctColors >= fx.minDistinctColors);
        }
    }
}

// --- Determinism: rendering the same frame twice is bit-identical ----------

TEST(golden_render_is_deterministic) {
    for (const auto& fx : golden::allFixtures()) {
        for (std::size_t frame : fx.goldenFrames) {
            std::vector<std::uint32_t> first, second;
            CHECK(renderGoldenFrame(fx, frame, first));
            CHECK(renderGoldenFrame(fx, frame, second));
            if (first.empty() || second.empty()) continue;
            CHECK(first.size() == second.size());
            CHECK(std::memcmp(first.data(), second.data(),
                              first.size() * sizeof(std::uint32_t)) == 0);
        }
    }
}

// --- Android R<->B swap round-trips every golden, with a padded stride ------
//
// Exercises the exact transform nativeCopyFrontInto applies (AndroidPixelConvert.h,
// Chunk 3.1), over every golden this chunk owns rather than just pixel-probe's
// single Chunk 0.4 golden — a wider-stride destination (as a real Bitmap's
// usually is) followed by swapping back must reproduce the source exactly,
// since the swap is involutive and padding bytes must never be touched twice.
TEST(golden_android_swap_round_trips) {
    using rnrlottie::rlottieSurfaceToAndroid;

    for (const auto& fx : golden::allFixtures()) {
        for (std::size_t frame : fx.goldenFrames) {
            const std::string goldenPath = golden::goldenFramePath(fx.name, frame);
            const auto src = golden::readRawFile(goldenPath);
            CHECK(src.size() == fx.width * fx.height * 4);
            if (src.size() != fx.width * fx.height * 4) continue;

            const std::size_t srcStride = fx.width * 4;
            const std::size_t dstStride = (fx.width + 5) * 4;  // padded, like a real Bitmap
            std::vector<std::uint8_t> android(dstStride * fx.height, 0xCD);
            rlottieSurfaceToAndroid(src.data(), srcStride, android.data(), dstStride, fx.width,
                                    fx.height);

            std::vector<std::uint8_t> roundTripped(srcStride * fx.height, 0);
            rlottieSurfaceToAndroid(android.data(), dstStride, roundTripped.data(), srcStride,
                                    fx.width, fx.height);

            CHECK(std::memcmp(roundTripped.data(), src.data(), src.size()) == 0);

            // Stride padding introduced by the wider destination is never
            // written past width*4 on any row.
            for (std::size_t y = 0; y < fx.height; ++y) {
                for (std::size_t b = fx.width * 4; b < dstStride; ++b) {
                    CHECK(android[y * dstStride + b] == 0xCD);
                }
            }
        }
    }
}

// --- GoldenCompare self-tests: a known-bad buffer must FAIL -----------------

TEST(golden_compare_byte_exact_self_test) {
    constexpr std::size_t w = 4, h = 4;
    std::vector<std::uint8_t> a(w * h * 4, 0);
    std::vector<std::uint8_t> b = a;

    auto same = golden::compareSurfaces(a.data(), w * 4, b.data(), w * 4, w, h,
                                        golden::ToleranceOptions{});
    CHECK(same.pass);
    CHECK(same.differingPixels == 0);

    // Corrupt exactly one pixel's red channel by a large delta.
    b[(2 * w + 1) * 4 + rnrlottie::kByteOffsetRed] =
        static_cast<std::uint8_t>(a[(2 * w + 1) * 4 + rnrlottie::kByteOffsetRed] + 100);
    auto bad = golden::compareSurfaces(a.data(), w * 4, b.data(), w * 4, w, h,
                                       golden::ToleranceOptions{});
    CHECK(!bad.pass);  // byte-exact tolerance must reject even one differing pixel
    CHECK(bad.differingPixels == 1);
    CHECK(bad.worstDelta.red == 100);
    CHECK(bad.hasDiff);
    CHECK(bad.firstDiffX == 1 && bad.firstDiffY == 2);

    // The same buffer passes under a tolerance wide enough to cover it...
    golden::ToleranceOptions loose;
    loose.maxPerChannelDelta = 100;
    loose.maxDifferingFraction = 1.0;
    CHECK(golden::compareSurfaces(a.data(), w * 4, b.data(), w * 4, w, h, loose).pass);

    // ...but a per-channel bound just one short of the real delta still fails.
    golden::ToleranceOptions tooTight;
    tooTight.maxPerChannelDelta = 99;
    tooTight.maxDifferingFraction = 1.0;
    CHECK(!golden::compareSurfaces(a.data(), w * 4, b.data(), w * 4, w, h, tooTight).pass);

    // A fraction bound of 0 rejects even a single differing pixel regardless
    // of how small the per-channel delta is.
    golden::ToleranceOptions zeroFraction;
    zeroFraction.maxPerChannelDelta = 255;
    zeroFraction.maxDifferingFraction = 0.0;
    CHECK(!golden::compareSurfaces(a.data(), w * 4, b.data(), w * 4, w, h, zeroFraction).pass);
}

TEST(golden_compare_non_vacuity_self_test) {
    constexpr std::size_t w = 4, h = 4;
    std::vector<std::uint8_t> blank(w * h * 4, 0);
    auto blankStats = golden::computeNonVacuity(blank.data(), w * 4, w, h);
    CHECK(blankStats.nonTransparentFraction() == 0.0);
    CHECK(blankStats.distinctColors == 1);  // all-zero is one distinct color

    std::vector<std::uint8_t> mixed = blank;
    mixed[0 * 4 + rnrlottie::kByteOffsetAlpha] = 255;
    mixed[0 * 4 + rnrlottie::kByteOffsetRed] = 200;
    auto mixedStats = golden::computeNonVacuity(mixed.data(), w * 4, w, h);
    CHECK(mixedStats.nonTransparentPixels == 1);
    CHECK(mixedStats.distinctColors == 2);
}
