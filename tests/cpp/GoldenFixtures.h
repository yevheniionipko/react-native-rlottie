// Chunk 7.4 (Part A) — the ONE fixture table for cross-platform golden
// verification. Host (golden_gen.cpp / golden_tests.cpp) and the forthcoming
// iOS-simulator / Android-emulator runners (Parts B/C) all read THIS table —
// never a hand-copied subset — so a fixture added or a golden-frame index
// changed here is automatically what every platform checks. A per-platform
// copy is exactly the kind of drift hazard CLAUDE.md's InputLimits note
// (Chunk 5.1) already warns about; this table exists so golden coverage
// can't silently diverge the same way.
//
// Header-only, dependency-free (std only) — see GoldenCompare.h's header
// comment for why.
#pragma once

#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

namespace rnrlottie::golden {

struct FixtureMarker {
    std::string name;
    std::size_t startFrame = 0;
    std::size_t endFrame = 0;
};

struct Fixture {
    const char* name;          // stable id; also the golden filename stem
    const char* relJsonPath;   // relative to dataRoot(), e.g. "fixtures/golden/shapes.json"
    std::vector<std::size_t> goldenFrames;  // frame indices verified byte-exact
    std::size_t width = 0;
    std::size_t height = 0;
    std::size_t totalFrames = 0;
    double duration = 0.0;         // seconds, +/- an epsilon at the call site
    std::vector<FixtureMarker> markers;  // empty if the fixture defines none

    // Per-fixture NON-VACUITY FLOORS, pinned from golden_gen.cpp's measured
    // output with margin (see the table below for each fixture's real value).
    //
    // A global "> 0 non-transparent pixels, > 1 color" floor is too weak to be
    // useful: the byte-exact golden check only protects a fixture as long as
    // nobody regenerates the goldens, and regeneration is exactly when a
    // fixture that has rotted into near-blankness (a renamed keypath, a mask
    // that stopped applying, an asset ref that no longer resolves) would slip
    // through — the new golden would agree with the new near-empty render, and
    // a >0% floor would wave it past. These per-fixture floors make that
    // regression a failure at regeneration time, which is the only moment it
    // is cheap to notice.
    double minNonTransparentFraction = 0.0;
    std::size_t minDistinctColors = 2;
};

// Plan §20 also lists text and image fixtures; both need external resources
// (a bundled font; an embedded or on-disk raster asset) that this corpus
// deliberately does not ship. A syntactically-valid Lottie a resource can't
// resolve renders as blank/missing content, which is exactly the vacuity
// trap this chunk exists to guard against elsewhere — so text/image golden
// coverage is an explicit, documented GAP rather than a fixture that would
// quietly pass by rendering nothing.
inline const std::vector<Fixture>& allFixtures() {
    // The trailing two numbers are the non-vacuity floors; the comment after
    // each row is the WORST value golden_gen.cpp actually measured across that
    // fixture's golden frames, so the margin is visible rather than implied.
    static const std::vector<Fixture> table = {
        // measured: 33.1% non-transparent, 123 distinct colors
        {"shapes", "fixtures/golden/shapes.json", {0, 10, 20, 30}, 64, 64, 31, 1.0, {}, 0.25, 40},
        // measured: 48.0%, 55 colors
        {"masks", "fixtures/golden/masks.json", {0, 15, 30}, 64, 64, 31, 1.0, {}, 0.35, 20},
        // measured: 100% (covers the whole canvas by design), 114 colors
        {"gradients", "fixtures/golden/gradients.json", {0, 15, 30}, 64, 64, 31, 1.0, {}, 0.90, 40},
        // measured: 59.8%, 8 colors (flat overlapping alpha bands — few colors
        // is correct here, so this floor is deliberately the lowest of the set)
        {"transparency", "fixtures/golden/transparency.json", {0, 15, 30}, 64, 64, 31, 1.0, {}, 0.45,
         6},
        // measured: 35.5%, 30 colors
        {"precomp", "fixtures/golden/precomp.json", {0, 15, 30}, 64, 64, 31, 1.0, {}, 0.25, 12},
        // measured: 11.7%, 23 colors (a small moving shape — low coverage is
        // the fixture's nature, not rot)
        {"markers",
         "fixtures/golden/markers.json",
         {0, 10, 25, 30},
         64,
         64,
         31,
         1.0,
         {
             {"start", 0, 0},
             {"mid", 10, 15},
             {"end", 25, 29},
         },
         0.08,
         10},
    };
    return table;
}

// Resolves where fixture JSON / golden .raw files live. Env override wins
// (device runners have a wholly different filesystem layout — an emulator
// pushes fixtures under /data/local/tmp or an app's assets dir, a simulator
// under its bundle resources — so a compiled-in absolute host path is
// meaningless there); otherwise falls back to the same compile-time macro
// the rest of tests/cpp already uses (RNRLOTTIE_TEST_DATA_DIR = "<repo>/tests"
// on the host).
inline std::string dataRoot() {
    if (const char* env = std::getenv("RNRLOTTIE_GOLDEN_DIR")) {
        if (env[0] != '\0') return std::string(env);
    }
#ifdef RNRLOTTIE_TEST_DATA_DIR
    return std::string(RNRLOTTIE_TEST_DATA_DIR);
#else
    return std::string("tests");
#endif
}

inline std::string fixtureJsonPath(const Fixture& f) {
    return dataRoot() + "/" + f.relJsonPath;
}

// tests/golden/<name>-frame<N>.argb32.raw — same directory pixel-probe's
// Chunk 0.4 golden already lives in, and the ONLY thing allowed to write
// there is golden_gen.cpp.
inline std::string goldenFramePath(const char* name, std::size_t frame) {
    return dataRoot() + "/golden/" + name + "-frame" + std::to_string(frame) + ".argb32.raw";
}

}  // namespace rnrlottie::golden
