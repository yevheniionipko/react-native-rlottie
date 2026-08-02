// Chunk 1.6 — ModelCacheController implementation.
#include "ModelCacheController.h"

#include <mutex>

#include <rlottie.h>

namespace rnrlottie {
namespace {

std::mutex& cacheMutex() {
    static std::mutex m;
    return m;
}

std::size_t& cacheSize() {
    static std::size_t size = ModelCacheController::kDefaultCacheSize;
    return size;
}

}  // namespace

void ModelCacheController::setModelCacheSize(std::size_t entries) {
    std::lock_guard<std::mutex> lk(cacheMutex());
    cacheSize() = entries;
    rlottie::configureModelCacheSize(entries);
}

void ModelCacheController::clearModelCache() {
    std::lock_guard<std::mutex> lk(cacheMutex());
    // rlottie has no explicit flush; sizing to 0 evicts everything. Restore the
    // configured size so caching stays enabled afterward.
    rlottie::configureModelCacheSize(0);
    rlottie::configureModelCacheSize(cacheSize());
}

std::size_t ModelCacheController::currentSize() {
    std::lock_guard<std::mutex> lk(cacheMutex());
    return cacheSize();
}

}  // namespace rnrlottie
