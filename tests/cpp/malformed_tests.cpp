// Chunk 5.5 — the malformed-Lottie corpus (tests/malformed/) run through the
// load path. Every file is genuinely hostile (truncated/invalid JSON, absurd
// or negative dimensions, pathological frame ranges, duplicate keys,
// NaN/Infinity tokens, unicode edge cases, a moderately large array field —
// see tests/malformed/ for the full set and cpp/InputLimits.h for the policy
// being exercised) but deliberately kept small on disk; the genuinely huge
// cases (oversize JSON, very deep nesting) are generated in-memory below
// instead of committed as multi-MB blobs.
//
// The bar for every case here is NOT "rejected" — a well-formed-but-unusual
// file may legitimately load. The bar is: the core reaches a CONSISTENT,
// well-typed state (isLoaded() agrees with the LoadResult; a failure has a
// real PlayerErrorCode; a "success" can be rendered without crashing) and,
// running under ASan/UBSan/TSan (tests/run-tests.sh), never crashes, hangs,
// or leaks.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "InputLimits.h"
#include "RlottiePlayerCore.h"
#include "test_harness.h"

using namespace rnrlottie;

namespace {

// Loads `json` (by value, as loadFromData consumes it) and asserts the core
// ends up in a consistent, typed state — the shared assertion every corpus
// entry and every synthetic case below runs through.
void checkLoadIsConsistentAndSafe(std::string json, const char* label) {
    RlottiePlayerCore core;
    auto r = core.loadFromData(json, label, "", /*useModelCache=*/false);

    CHECK(r.success == core.isLoaded());
    if (!r.success) {
        // A rejection must always be a real, typed error — never PlayerErrorCode::None
        // masquerading as success, and never left for the caller to guess at.
        CHECK(r.error.code != PlayerErrorCode::None);
        CHECK(core.lastError().code == r.error.code);
        return;
    }

    // A "success" must be safely renderable, even from attacker-controlled
    // declared dimensions/markers — renderFrame() targets a buffer sized from
    // OUR clamped choice, not from whatever the JSON claimed, so this cannot
    // itself become an oversized allocation.
    constexpr std::size_t kW = 8, kH = 8;
    std::vector<std::uint32_t> buf(kW * kH, 0);
    core.renderFrame(0, buf.data(), kW, kH, kW * 4, /*preserveAspectRatio=*/true);
    // Any frame index, including ones derived from an absurd declared frame
    // count, must clamp rather than crash.
    core.renderFrame(SIZE_MAX / 2, buf.data(), kW, kH, kW * 4, true);
}

}  // namespace

TEST(malformed_corpus_never_crashes) {
    const std::filesystem::path dir =
        std::filesystem::path(tst::dataPath("malformed"));
    std::size_t filesChecked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        const std::string path = entry.path().string();
        const std::string text = tst::readText(path);

        checkLoadIsConsistentAndSafe(text, path.c_str());

        // Also exercise the File-kind path (stat()-based pre-parse size gate +
        // rlottie's own file reader) for the same bytes.
        RlottiePlayerCore fileCore;
        auto fr = fileCore.loadFromFile(path, /*useModelCache=*/false);
        CHECK(fr.success == fileCore.isLoaded());
        if (!fr.success) {
            CHECK(fr.error.code != PlayerErrorCode::None);
        }
        ++filesChecked;
    }
    // Guards against the corpus directory silently going empty (e.g. a path
    // typo) and this test passing vacuously.
    CHECK(filesChecked >= 10);
}

TEST(malformed_oversize_json_rejected_without_reaching_rlottie) {
    tst::ScopedInputLimits restore;
    InputLimits tight;
    tight.maxJsonBytes = 4096;
    setInputLimits(tight);

    // Cheap to build (a few KB), well over the configured limit — proves the
    // byte-size gate runs before rlottie tokenizes anything, independent of
    // whether the content would otherwise parse.
    std::string json = R"({"v":"5.7.0","fr":30,"ip":0,"op":30,"w":64,"h":64,"nm":")";
    json.append(tight.maxJsonBytes, 'A');
    json += "\",\"assets\":[],\"layers\":[]}";

    RlottiePlayerCore core;
    auto r = core.loadFromData(json, "oversize", "", false);
    CHECK(!r.success);
    CHECK(r.error.code == PlayerErrorCode::SourceTooLarge);
    CHECK(!core.isLoaded());
}

TEST(malformed_deeply_nested_groups_do_not_crash) {
    // rlottie's bundled rapidjson parses with recursive-descent flags, so deep
    // nesting recurses once per level.
    //
    // The original rationale here claimed maxJsonBytes bounds this. IT DOES
    // NOT, and the assumption was measured to be wrong: the nesting token below
    // is ~18 bytes, so the 16 MiB default still admits ~930k levels, and depth
    // 100,000 — about 1.9 MB, ~11% of the byte limit — reproducibly SIGSEGV'd a
    // normal 8 MiB main-thread stack. Parsing actually happens on a render
    // worker, where stacks are typically 512 KB, so the real margin is smaller.
    //
    // `checkJsonDepth` (cpp/InputLimits.h) now rejects over-deep JSON before
    // rlottie's parser sees it; `malformed_over_deep_json_rejected_not_crash`
    // below is the guard's regression test. This case stays as the "legal but
    // unusual" side of the boundary: deep enough to be pathological, still
    // under maxJsonDepth, so it must parse rather than be rejected.
    constexpr int kDepth = 400;
    std::string json = R"({"v":"5.7.0","fr":30,"ip":0,"op":30,"w":64,"h":64,"nm":"deep",)";
    json += R"("assets":[],"layers":[{"ty":4,"nm":"g","sr":1,"ks":{},"ind":1,"shapes":[)";
    for (int i = 0; i < kDepth; ++i) {
        json += R"({"ty":"gr","it":[)";
    }
    json += "{}";
    for (int i = 0; i < kDepth; ++i) {
        json += "]}";
    }
    json += "]}]}";

    checkLoadIsConsistentAndSafe(std::move(json), "deep-nesting");
}

// Chunk 5.1 regression guard: JSON nested past maxJsonDepth must be REJECTED
// with a typed error, not crash. Before checkJsonDepth() existed, this input
// (well under maxJsonBytes) killed the process with SIGSEGV via rapidjson's
// recursive descent — a hostile animation file could take down the whole app,
// which plan §16 forbids. A depth just over the limit proves the boundary; the
// far deeper case proves the guard runs BEFORE any recursion, so cost is linear
// in bytes rather than in stack frames.
TEST(malformed_over_deep_json_rejected_not_crash) {
    const InputLimits limits = currentInputLimits();

    for (const std::size_t depth :
         {limits.maxJsonDepth + 1, limits.maxJsonDepth * 8, std::size_t{100000}}) {
        std::string json = R"({"v":"5.7.0","fr":30,"ip":0,"op":30,"w":64,"h":64,)";
        json += R"("assets":[],"layers":[{"ty":4,"nm":"g","sr":1,"ks":{},"ind":1,"shapes":[)";
        for (std::size_t i = 0; i < depth; ++i) json += R"({"ty":"gr","it":[)";
        json += "{}";
        for (std::size_t i = 0; i < depth; ++i) json += "]}";
        json += "]}]}";

        // Under the byte limit — so ONLY the depth guard can be what stops it.
        CHECK(json.size() <= limits.maxJsonBytes);

        RlottiePlayerCore core;
        const auto r = core.loadFromData(json, "deep", "", /*useModelCache=*/false);
        CHECK(!r.success);
        CHECK(r.error.code == PlayerErrorCode::ParseFailed);
        CHECK(!core.isLoaded());
    }
}

// Brackets inside JSON STRINGS are not structure. A guard that counted them
// would reject legitimate animations whose text/name fields contain brackets —
// a false positive that would look like a mysterious PARSE_FAILED in the field.
TEST(malformed_depth_guard_ignores_brackets_in_strings) {
    InputLimits limits = currentInputLimits();
    limits.maxJsonDepth = 8;

    std::string brackets;
    for (int i = 0; i < 200; ++i) brackets += "[{";
    // Also exercise an escaped quote, so the scanner cannot mistake it for the
    // string's end and start treating the payload as structure.
    // The \" below is a literal backslash-quote in the JSON text: an ESCAPED
    // quote that must NOT be read as the string's terminator. (Writing a bare "
    // here would genuinely close the string, making the brackets that follow
    // real structure — and rejecting those would be correct, which is exactly
    // how an earlier version of this test fooled itself.)
    const std::string json = std::string(R"({"nm":")") + brackets +
                             R"(\" still in string )" + brackets + R"("})";

    CHECK(!checkJsonDepth(json, limits));

    // The counterpart: once the string really does end, brackets ARE structure
    // and must still be counted — a scanner that stayed "in string" forever
    // would silently disable the guard.
    const std::string closed = std::string(R"({"nm":"x")") + "," + brackets;
    CHECK(checkJsonDepth(closed, limits));
}
