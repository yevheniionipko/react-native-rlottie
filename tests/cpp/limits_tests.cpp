// Chunk 1.7 — InputLimits primitives + ModelCacheController.
#include "InputLimits.h"
#include "ModelCacheController.h"
#include "test_harness.h"

using namespace rnrlottie;

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

TEST(model_cache_controller) {
    CHECK(ModelCacheController::currentSize() == ModelCacheController::kDefaultCacheSize);
    ModelCacheController::setModelCacheSize(16);
    CHECK(ModelCacheController::currentSize() == 16);
    ModelCacheController::clearModelCache();
    CHECK(ModelCacheController::currentSize() == 16);  // preserved across flush
    ModelCacheController::setModelCacheSize(0);
    CHECK(ModelCacheController::currentSize() == 0);
}
