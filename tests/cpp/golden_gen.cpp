// Chunk 7.4 (Part A) — regenerates tests/golden/<name>-frame<N>.argb32.raw
// from the ONE fixture table (GoldenFixtures.h). This is the ONLY thing
// allowed to WRITE those files; golden_tests.cpp only ever reads them.
//
// Own main() (like fuzz_loadfromdata.cpp / bench_render.cpp) — an opt-in tool,
// not part of the default test binary. Invoke via `tests/run-tests.sh
// golden-gen`. Re-run and re-commit the .raw outputs whenever a golden
// fixture's rendered content is meant to change; otherwise golden_tests.cpp's
// byte-exact comparison exists precisely to catch an unintended change.
//
// For every golden frame this prints:
//   - its sha256 (cpp/Sha256.h) so a re-generation's diff is auditable from
//     the log alone, without opening the binary,
//   - non-vacuity stats (fraction of non-transparent pixels, distinct color
//     count) using the SAME rules golden_tests.cpp asserts on, so "does this
//     fixture actually render something" is decided by identical code in the
//     generator and the gate rather than by eyeballing the log.
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "GoldenCompare.h"
#include "GoldenFixtures.h"
#include "RlottiePlayerCore.h"
#include "Sha256.h"

namespace {

using rnrlottie::RlottiePlayerCore;
namespace golden = rnrlottie::golden;

std::string readTextFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "golden_gen: cannot open %s\n", path.c_str());
        std::exit(2);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeRawFile(const std::string& path, const std::uint8_t* data, std::size_t bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    return f.good();
}

int runFixture(const golden::Fixture& fx) {
    int failures = 0;
    const std::string json = readTextFile(golden::fixtureJsonPath(fx));

    RlottiePlayerCore core;
    auto load = core.loadFromData(json, std::string("golden-gen-") + fx.name, "", false);
    if (!load.success) {
        std::fprintf(stderr, "golden_gen: %s failed to load (code=%d): %s\n", fx.name,
                     static_cast<int>(load.error.code), load.error.message.c_str());
        return 1;
    }

    std::printf("== %s ==\n", fx.name);
    std::printf("  metadata: %zux%zu  totalFrames=%zu  duration=%.3f  markers=%zu\n",
                load.metadata.width, load.metadata.height, load.metadata.totalFrames,
                load.metadata.duration, load.metadata.markers.size());
    if (load.metadata.width != fx.width || load.metadata.height != fx.height ||
        load.metadata.totalFrames != fx.totalFrames) {
        std::fprintf(stderr,
                     "  WARNING: metadata does not match GoldenFixtures.h's table "
                     "(expected %zux%zu, totalFrames=%zu) — update the table.\n",
                     fx.width, fx.height, fx.totalFrames);
        ++failures;
    }

    std::vector<std::uint32_t> pixels(fx.width * fx.height, 0);
    const std::size_t stride = fx.width * 4;

    for (std::size_t frame : fx.goldenFrames) {
        std::fill(pixels.begin(), pixels.end(), 0u);
        if (!core.renderFrame(frame, pixels.data(), fx.width, fx.height, stride, true)) {
            std::fprintf(stderr, "  frame %zu: renderFrame FAILED (%s)\n", frame,
                         load.error.message.c_str());
            ++failures;
            continue;
        }

        const auto* bytes = reinterpret_cast<const std::uint8_t*>(pixels.data());
        const std::size_t byteCount = pixels.size() * sizeof(std::uint32_t);

        const std::string outPath = golden::goldenFramePath(fx.name, frame);
        if (!writeRawFile(outPath, bytes, byteCount)) {
            std::fprintf(stderr, "  frame %zu: failed to write %s\n", frame, outPath.c_str());
            ++failures;
            continue;
        }

        const std::string hash =
            rnrlottie::sha256Hex(std::string(reinterpret_cast<const char*>(bytes), byteCount));
        const auto stats = golden::computeNonVacuity(bytes, stride, fx.width, fx.height);
        std::printf(
            "  frame %-3zu sha256=%s  nonTransparent=%5.1f%%  distinctColors=%4zu  -> %s\n", frame,
            hash.c_str(), 100.0 * stats.nonTransparentFraction(), stats.distinctColors,
            outPath.c_str());

        // The whole point of a golden gate is a REAL comparison target. A
        // frame that is entirely transparent (or one flat color) would make
        // golden_tests.cpp's byte-exact check pass vacuously for ANY renderer
        // bug that also produces a blank frame, so refuse to ship such a
        // fixture silently.
        //
        // The floors are the per-fixture ones pinned in GoldenFixtures.h rather
        // than a global ">0 pixels, >1 color": regeneration is precisely when a
        // fixture that has rotted into near-blankness would otherwise be
        // blessed into a new golden (see Fixture::minNonTransparentFraction).
        if (stats.nonTransparentFraction() < fx.minNonTransparentFraction) {
            std::fprintf(stderr,
                         "  frame %zu: VACUOUS — %.1f%% non-transparent, below this fixture's "
                         "%.1f%% floor, refusing\n",
                         frame, 100.0 * stats.nonTransparentFraction(),
                         100.0 * fx.minNonTransparentFraction);
            ++failures;
        } else if (stats.distinctColors < fx.minDistinctColors) {
            std::fprintf(stderr,
                         "  frame %zu: VACUOUS — only %zu distinct color(s), below this fixture's "
                         "floor of %zu\n",
                         frame, stats.distinctColors, fx.minDistinctColors);
            ++failures;
        }
    }

    return failures;
}

}  // namespace

int main() {
    std::printf("=== Chunk 7.4 (Part A) golden regeneration ===\n");
    std::printf("data root: %s\n\n", golden::dataRoot().c_str());

    int totalFailures = 0;
    for (const auto& fx : golden::allFixtures()) {
        totalFailures += runFixture(fx);
        std::printf("\n");
    }

    if (totalFailures == 0) {
        std::printf("=== done, no vacuous fixtures ===\n");
        return 0;
    }
    std::fprintf(stderr, "=== %d problem(s) — see above ===\n", totalFailures);
    return 1;
}
