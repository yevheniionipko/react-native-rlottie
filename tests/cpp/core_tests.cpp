// Chunk 1.7 — RlottiePlayerCore + golden-frame tests (plan §20).
#include <cstring>
#include <limits>
#include <vector>

#include "RlottiePlayerCore.h"
#include "test_harness.h"

using namespace rnrlottie;

static std::string fixture() {
    return tst::readText(tst::dataPath("fixtures/pixel-probe.json"));
}

// Chunk 6.1 — a small inline (not tests/fixtures/, which this stream does not
// own) 64x64 shape layer: a full-canvas red fill + a blue 2px stroke, both at
// 100% opacity. Used to verify setOpacity/setStrokeWidth actually reach
// rlottie and visibly change the rendered pixels, not just "doesn't crash".
//
// IMPORTANT: the layer opacity below carries a (near-invisible, 100->99%)
// KEYFRAME ANIMATION rather than a flat "a":0 value. This is load-bearing,
// not incidental: rlottie's renderer::Layer::update() (lottieitem.cpp) skips
// re-evaluating a layer's paint content on frames where the layer itself is
// static (mLayerData->isStatic()) and no dirty flag is set, REGARDLESS of a
// setValue() override added afterward — verified experimentally against this
// vendored rlottie build. A fully-static ("a":0 everywhere) fixture renders
// its filter override into the model correctly (confirmed via FilterData)
// but the drawable is never rebuilt, so the override never reaches pixels on
// repeat renders of the same static content. Since this stream does not own
// cpp/third_party/rlottie/ (vendored+pinned) or the shipped setColor's
// existing tests, this fixture works around it rather than fixing rlottie;
// see the Phase 6 report for the full writeup and its implications for
// setColor too (a pre-existing characteristic, not something this phase
// introduced).
//
// Shape order note: rlottie reverses "it" (bodymovin's front-to-back array
// order) into back-to-front draw order (Group::addChildren, lottieitem.cpp),
// so the FIRST paint item in "it" is drawn LAST (on top). The stroke is
// listed before the fill below so the stroke is visible as an outline rather
// than being fully covered by the fill drawn over it.
static std::string shapeFixture() {
    return R"JSON({
      "v": "5.7.0", "fr": 30, "ip": 0, "op": 30, "w": 64, "h": 64,
      "nm": "opacity-stroke-probe", "ddd": 0, "assets": [],
      "layers": [
        {
          "ddd": 0, "ind": 1, "ty": 4, "nm": "fill-square", "sr": 1,
          "ks": {
            "o": {"a": 1, "k": [
              {"t": 0, "s": [100], "i": {"x": [0.667], "y": [1]}, "o": {"x": [0.333], "y": [0]}},
              {"t": 29, "s": [99]}
            ]},
            "r": {"a": 0, "k": 0},
            "p": {"a": 0, "k": [32, 32, 0]}, "a": {"a": 0, "k": [0, 0, 0]},
            "s": {"a": 0, "k": [100, 100, 100]}
          },
          "ip": 0, "op": 30, "st": 0, "bm": 0,
          "shapes": [
            {
              "ty": "gr", "nm": "grp",
              "it": [
                {"ty": "rc", "nm": "rect", "p": {"a": 0, "k": [0, 0]},
                 "s": {"a": 0, "k": [64, 64]}, "r": {"a": 0, "k": 0}},
                {"ty": "st", "nm": "stroke", "c": {"a": 0, "k": [0, 0, 1, 1]},
                 "o": {"a": 0, "k": 100}, "w": {"a": 0, "k": 2}},
                {"ty": "fl", "nm": "fill", "c": {"a": 0, "k": [1, 0, 0, 1]},
                 "o": {"a": 0, "k": 100}},
                {"ty": "tr", "p": {"a": 0, "k": [0, 0]}, "a": {"a": 0, "k": [0, 0]},
                 "s": {"a": 0, "k": [100, 100]}, "r": {"a": 0, "k": 0}, "o": {"a": 0, "k": 100}}
              ]
            }
          ]
        }
      ]
    })JSON";
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

TEST(core_setopacity_no_crash_when_not_loaded) {
    RlottiePlayerCore core;
    core.setOpacity("**", 0.5f);  // no animation loaded: must be a silent no-op
    CHECK(!core.isLoaded());
}

TEST(core_setopacity_lowers_center_alpha) {
    RlottiePlayerCore core;
    CHECK(core.loadFromData(shapeFixture(), "p", "", false).success);
    std::vector<std::uint32_t> before(64 * 64, 0);
    CHECK(core.renderFrame(0, before.data(), 64, 64, 256, true));
    const std::uint32_t centerBefore = before[32 * 64 + 32];
    CHECK(((centerBefore >> 24) & 0xFF) == 0xFF);  // fully opaque at 100%

    core.setOpacity("**", 0.5f);
    std::vector<std::uint32_t> after(64 * 64, 0);
    CHECK(core.renderFrame(0, after.data(), 64, 64, 256, true));
    const std::uint32_t centerAfter = after[32 * 64 + 32];
    const int alphaAfter = (centerAfter >> 24) & 0xFF;
    // Premultiplied ARGB32: halving fill opacity roughly halves alpha (and the
    // premultiplied color channels with it). Allow slack for rounding.
    CHECK(alphaAfter > 100 && alphaAfter < 155);
}

TEST(core_setopacity_clamps_out_of_range_and_nan) {
    RlottiePlayerCore core;
    CHECK(core.loadFromData(shapeFixture(), "p", "", false).success);
    std::vector<std::uint32_t> buf(64 * 64, 0);
    // Out-of-range and NaN must clamp rather than corrupt/crash rendering.
    core.setOpacity("**", -5.0f);
    CHECK(core.renderFrame(0, buf.data(), 64, 64, 256, true));
    CHECK(((buf[32 * 64 + 32] >> 24) & 0xFF) == 0);  // clamped to 0 => transparent

    core.setOpacity("**", 999.0f);
    CHECK(core.renderFrame(0, buf.data(), 64, 64, 256, true));
    CHECK(((buf[32 * 64 + 32] >> 24) & 0xFF) == 0xFF);  // clamped to 1 => opaque

    core.setOpacity("**", std::numeric_limits<float>::quiet_NaN());
    CHECK(core.renderFrame(0, buf.data(), 64, 64, 256, true));  // no crash/UB
}

TEST(core_setstrokewidth_reaches_rlottie_no_crash) {
    RlottiePlayerCore core;
    CHECK(core.loadFromData(shapeFixture(), "p", "", false).success);
    std::vector<std::uint32_t> buf(64 * 64, 0);
    core.setStrokeWidth("**", 10.0f);
    CHECK(core.renderFrame(0, buf.data(), 64, 64, 256, true));
    // A pixel 1px in from the edge is inside the original 2px stroke band, but
    // with the stroke widened to 10px it should now read blue-ish rather than
    // the pure interior red.
    const std::uint32_t p = buf[1 * 64 + 32];
    const int b = p & 0xFF;
    const int r = (p >> 16) & 0xFF;
    CHECK(b > r);
}

TEST(core_setstrokewidth_clamps_nonpositive_and_nan) {
    RlottiePlayerCore core;
    CHECK(core.loadFromData(shapeFixture(), "p", "", false).success);
    std::vector<std::uint32_t> buf(64 * 64, 0);
    core.setStrokeWidth("**", -3.0f);
    CHECK(core.renderFrame(0, buf.data(), 64, 64, 256, true));  // clamps, no crash
    core.setStrokeWidth("**", 0.0f);
    CHECK(core.renderFrame(0, buf.data(), 64, 64, 256, true));
    core.setStrokeWidth("**", std::numeric_limits<float>::quiet_NaN());
    CHECK(core.renderFrame(0, buf.data(), 64, 64, 256, true));  // no crash/UB
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
