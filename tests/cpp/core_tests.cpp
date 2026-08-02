// Chunk 1.7 — RlottiePlayerCore + golden-frame tests (plan §20).
#include <cstring>
#include <vector>

#include "RlottiePlayerCore.h"
#include "test_harness.h"

using namespace rnrlottie;

static std::string fixture() {
    return tst::readText(tst::dataPath("fixtures/pixel-probe.json"));
}

TEST(core_load_valid_metadata) {
    RlottiePlayerCore core;
    CHECK(!core.isLoaded());
    auto r = core.loadFromData(fixture(), "probe", "", false);
    CHECK(r.success);
    CHECK(core.isLoaded());
    CHECK(r.metadata.width == 64 && r.metadata.height == 64);
    CHECK(r.metadata.totalFrames == 31);
    CHECK(r.metadata.frameRate > 29.0 && r.metadata.frameRate < 31.0);
    CHECK(r.metadata.duration > 0.9 && r.metadata.duration < 1.1);
}

TEST(core_invalid_json) {
    RlottiePlayerCore core;
    auto r = core.loadFromData("{ not lottie", "bad", "", false);
    CHECK(!r.success);
    CHECK(r.error.code == PlayerErrorCode::ParseFailed);
    CHECK(!core.isLoaded());
}

TEST(core_empty_object_does_not_crash) {
    RlottiePlayerCore core;
    auto r = core.loadFromData("{}", "empty", "", false);
    // Either outcome is acceptable; must not crash and state must be consistent.
    CHECK(r.success == core.isLoaded());
}

TEST(core_render_preconditions) {
    RlottiePlayerCore core;
    std::vector<std::uint32_t> buf(64 * 64, 0);

    // render before load
    CHECK(!core.renderFrame(0, buf.data(), 64, 64, 256, true));
    CHECK(core.lastError().code == PlayerErrorCode::RenderFailed);

    CHECK(core.loadFromData(fixture(), "p", "", false).success);
    CHECK(!core.renderFrame(0, nullptr, 64, 64, 256, true));
    CHECK(core.lastError().code == PlayerErrorCode::InvalidDimensions);
    CHECK(!core.renderFrame(0, buf.data(), 0, 64, 0, true));
    CHECK(core.lastError().code == PlayerErrorCode::InvalidDimensions);
    CHECK(!core.renderFrame(0, buf.data(), 64, 64, 255, true));  // stride < w*4
    CHECK(core.lastError().code == PlayerErrorCode::InvalidDimensions);
    CHECK(core.renderFrame(9999, buf.data(), 64, 64, 256, true));  // clamped, not rejected
}

TEST(core_frame_for_progress_clamps) {
    RlottiePlayerCore core;
    CHECK(core.frameForProgress(0.5) == 0);  // not loaded
    CHECK(core.loadFromData(fixture(), "p", "", false).success);
    CHECK(core.frameForProgress(-5.0) == 0);
    const auto high = core.frameForProgress(1.0);
    CHECK(core.frameForProgress(2.0) == high);
}

TEST(core_setcolor_keypath_does_not_crash) {
    RlottiePlayerCore core;
    CHECK(core.loadFromData(fixture(), "p", "", false).success);
    core.setColor("**", 0.0f, 1.0f, 0.0f);  // wildcard key path
    std::vector<std::uint32_t> buf(64 * 64, 0);
    CHECK(core.renderFrame(0, buf.data(), 64, 64, 256, true));
}

TEST(core_source_replacement_drops_old) {
    RlottiePlayerCore core;
    CHECK(core.loadFromData(fixture(), "a", "", false).success);
    CHECK(!core.loadFromData("garbage", "b", "", false).success);
    CHECK(!core.isLoaded());  // failed replacement leaves no stale animation
}

TEST(core_golden_frame0) {
    RlottiePlayerCore core;
    CHECK(core.loadFromData(fixture(), "g", "", false).success);
    std::vector<std::uint32_t> buf(64 * 64, 0xDEADBEEF);
    CHECK(core.renderFrame(0, buf.data(), 64, 64, 256, true));

    const auto golden = tst::readBinary(tst::dataPath("golden/pixel-probe-frame0.argb32.raw"));
    CHECK(golden.size() == 64 * 64 * 4);
    if (golden.size() == 64 * 64 * 4) {
        CHECK(std::memcmp(buf.data(), golden.data(), golden.size()) == 0);
    }
}
