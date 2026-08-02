// Chunk 1.7 / 5.1 — InputLimits primitives, the process-global config seam,
// and ModelCacheController.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "InputLimits.h"
#include "ModelCacheController.h"
#include "RlottiePlayerCore.h"
#include "test_harness.h"

using namespace rnrlottie;

namespace {

std::string fixture() { return tst::readText(tst::dataPath("fixtures/pixel-probe.json")); }

}  // namespace

TEST(limits_json_size) {
    InputLimits lim;
    CHECK(!checkJsonSize(1024, lim));
    CHECK(!checkJsonSize(lim.maxJsonBytes, lim));
    CHECK(checkJsonSize(lim.maxJsonBytes + 1, lim).code == PlayerErrorCode::SourceTooLarge);
}

TEST(limits_frame_count) {
    InputLimits lim;
    CHECK(!checkFrameCount(31, lim));
    CHECK(!checkFrameCount(lim.maxFrames, lim));
    CHECK(checkFrameCount(lim.maxFrames + 1, lim).code == PlayerErrorCode::SourceTooLarge);
}

TEST(limits_dimensions) {
    InputLimits lim;  // maxPixels defaults to 4096*4096
    CHECK(!checkDimensions(64, 64, lim));
    CHECK(checkDimensions(0, 64, lim).code == PlayerErrorCode::InvalidDimensions);
    CHECK(checkDimensions(64, 0, lim).code == PlayerErrorCode::InvalidDimensions);
    CHECK(checkDimensions(5000, 5000, lim).code == PlayerErrorCode::InvalidDimensions);  // over maxPixels
    CHECK(checkDimensions(SIZE_MAX, 2, lim).code == PlayerErrorCode::InvalidDimensions);  // overflow
    CHECK(checkDimensions(static_cast<std::size_t>(UINT32_MAX) + 1, 2, lim).code ==
          PlayerErrorCode::InvalidDimensions);  // axis exceeds 32-bit range
}

TEST(limits_global_config_default_and_roundtrip) {
    tst::ScopedInputLimits restore;
    // Defaults match InputLimits{} until configured (Chunk 5.1 seam contract).
    setInputLimits(InputLimits{});
    CHECK(currentInputLimits().maxJsonBytes == InputLimits{}.maxJsonBytes);

    InputLimits custom;
    custom.maxJsonBytes = 1024;
    custom.maxPixels = 100;
    custom.maxFrames = 5;
    setInputLimits(custom);
    const InputLimits got = currentInputLimits();
    CHECK(got.maxJsonBytes == 1024);
    CHECK(got.maxPixels == 100);
    CHECK(got.maxFrames == 5);
}

// --- Chunk 5.1: the core actually enforces these limits, not just the pure
// helpers above. ---------------------------------------------------------

TEST(core_enforces_json_size_limit) {
    tst::ScopedInputLimits restore;
    const std::string json = fixture();
    InputLimits tight;
    tight.maxJsonBytes = json.size() - 1;  // one byte too small
    setInputLimits(tight);

    RlottiePlayerCore core;
    auto r = core.loadFromData(json, "k", "", false);
    CHECK(!r.success);
    CHECK(r.error.code == PlayerErrorCode::SourceTooLarge);
    CHECK(!core.isLoaded());
}

TEST(core_enforces_frame_count_limit) {
    tst::ScopedInputLimits restore;
    InputLimits tight;
    tight.maxFrames = 1;  // pixel-probe.json declares 31 frames
    setInputLimits(tight);

    RlottiePlayerCore core;
    auto r = core.loadFromData(fixture(), "k", "", false);
    CHECK(!r.success);
    CHECK(r.error.code == PlayerErrorCode::SourceTooLarge);
    CHECK(!core.isLoaded());  // rlottie parsed it internally, but it is never exposed
}

TEST(core_enforces_composition_dimension_limit) {
    tst::ScopedInputLimits restore;
    InputLimits tight;
    tight.maxPixels = 64 * 64 - 1;  // pixel-probe.json is a 64x64 composition
    setInputLimits(tight);

    RlottiePlayerCore core;
    auto r = core.loadFromData(fixture(), "k", "", false);
    CHECK(!r.success);
    CHECK(r.error.code == PlayerErrorCode::InvalidDimensions);
    CHECK(!core.isLoaded());
}

TEST(core_within_limits_still_loads) {
    tst::ScopedInputLimits restore;
    setInputLimits(InputLimits{});  // generous defaults
    RlottiePlayerCore core;
    CHECK(core.loadFromData(fixture(), "k", "", false).success);
}

TEST(model_cache_controller) {
    CHECK(ModelCacheController::currentSize() == ModelCacheController::kDefaultCacheSize);
    ModelCacheController::setModelCacheSize(16);
    CHECK(ModelCacheController::currentSize() == 16);
    ModelCacheController::clearModelCache();
    CHECK(ModelCacheController::currentSize() == 16);  // preserved across flush
    ModelCacheController::setModelCacheSize(0);
    CHECK(ModelCacheController::currentSize() == 0);
}
