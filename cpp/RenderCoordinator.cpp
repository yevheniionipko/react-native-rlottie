// Chunk 1.5 — RenderCoordinator implementation.
#include "RenderCoordinator.h"

#include <utility>

namespace rnrlottie {

RenderCoordinator::RenderCoordinator(FrameSink& sink, FrameBuffer::Limits limits)
    : sink_(sink),
      frameBuffer_(limits),
      core_(std::make_unique<RlottiePlayerCore>()) {
    worker_ = std::thread([this] { workerLoop(); });
}

RenderCoordinator::~RenderCoordinator() { release(); }

std::uint64_t RenderCoordinator::setSource(AnimationSource source) {
    std::uint64_t g;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        g = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        const std::uint64_t epoch =
            sourceEpoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (stop_) return g;
        pendingFrame_.reset();  // stale render for the previous source
        controlQueue_.push_back([this, s = std::move(source), epoch]() mutable {
            runLoad(std::move(s), epoch);
        });
    }
    cv_.notify_one();
    return g;
}

std::uint64_t RenderCoordinator::setSurfaceSize(std::size_t width, std::size_t height) {
    std::uint64_t g;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        g = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (stop_) return g;
        pendingFrame_.reset();
        controlQueue_.push_back([this, width, height, g] { runResize(width, height, g); });
    }
    cv_.notify_one();
    return g;
}

void RenderCoordinator::requestFrame(std::size_t frame, std::uint64_t generation) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (stop_) return;
        pendingFrame_ = frame;  // latest-wins; no queue growth
        pendingGeneration_ = generation;
    }
    cv_.notify_one();
}

void RenderCoordinator::setColor(std::string keyPath, float red, float green, float blue) {
    const std::uint64_t epoch = sourceEpoch_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (stop_) return;
        controlQueue_.push_back([this, k = std::move(keyPath), red, green, blue, epoch]() mutable {
            runSetColor(std::move(k), red, green, blue, epoch);
        });
    }
    cv_.notify_one();
}

void RenderCoordinator::setOpacity(std::string keyPath, float opacity) {
    const std::uint64_t epoch = sourceEpoch_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (stop_) return;
        controlQueue_.push_back([this, k = std::move(keyPath), opacity, epoch]() mutable {
            runSetOpacity(std::move(k), opacity, epoch);
        });
    }
    cv_.notify_one();
}

void RenderCoordinator::setStrokeWidth(std::string keyPath, float width) {
    const std::uint64_t epoch = sourceEpoch_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (stop_) return;
        controlQueue_.push_back([this, k = std::move(keyPath), width, epoch]() mutable {
            runSetStrokeWidth(std::move(k), width, epoch);
        });
    }
    cv_.notify_one();
}

void RenderCoordinator::reset() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        generation_.fetch_add(1, std::memory_order_acq_rel);
        pendingFrame_.reset();
    }
    // No worker task: bumping the generation drops any in-flight result; the view
    // re-requests the start frame with the new generation.
}

void RenderCoordinator::release() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (released_) return;
        released_ = true;
        stop_ = true;
        generation_.fetch_add(1, std::memory_order_acq_rel);
        controlQueue_.clear();
        pendingFrame_.reset();
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

const FrameBuffer& RenderCoordinator::frameBuffer() const { return frameBuffer_; }

std::uint64_t RenderCoordinator::generation() const {
    return generation_.load(std::memory_order_acquire);
}

void RenderCoordinator::workerLoop() {
    for (;;) {
        std::function<void()> task;
        std::size_t frame = 0;
        std::uint64_t frameGen = 0;
        bool doRender = false;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this] {
                return stop_ || !controlQueue_.empty() || pendingFrame_.has_value();
            });
            if (stop_) {
                break;  // cancel remaining work
            }
            if (!controlQueue_.empty()) {
                task = std::move(controlQueue_.front());
                controlQueue_.pop_front();
            } else {
                frame = *pendingFrame_;
                frameGen = pendingGeneration_;
                pendingFrame_.reset();
                doRender = true;
            }
        }
        // Sink callbacks run here with the mutex released.
        if (task) {
            task();
        } else if (doRender) {
            runRender(frame, frameGen);
        }
    }
    // Destroy the rlottie animation on the worker thread (plan §1.5 / §5).
    core_.reset();
}

void RenderCoordinator::runLoad(AnimationSource source, std::uint64_t sourceEpoch) {
    if (sourceEpoch != sourceEpoch_.load(std::memory_order_acquire)) {
        return;  // a newer setSource superseded this one; skip the parse entirely
    }
    RlottiePlayerCore::LoadResult result =
        source.kind == AnimationSource::Kind::File
            ? core_->loadFromFile(source.path, source.useModelCache)
            : core_->loadFromData(std::move(source.json), source.cacheKey,
                                  source.resourcePath, source.useModelCache);
    if (sourceEpoch != sourceEpoch_.load(std::memory_order_acquire)) {
        return;  // superseded during the parse; drop the (now stale) event
    }
    PlayerEvent ev;
    if (result.success) {
        ev.type = PlayerEvent::Type::Loaded;
        ev.metadata = result.metadata;
    } else {
        ev.type = PlayerEvent::Type::Error;
        ev.error = result.error;
        ev.error.generation = generation_.load(std::memory_order_acquire);
    }
    sink_.onEvent(ev);
}

void RenderCoordinator::runResize(std::size_t width, std::size_t height,
                                  std::uint64_t generation) {
    // Executes unconditionally (FIFO) so the buffer is always set up, even if a
    // setSource bumped the global generation after this resize was enqueued.
    PlayerError err;
    if (!frameBuffer_.resize(FrameBuffer::Dimensions{width, height}, err)) {
        err.generation = generation;
        PlayerEvent ev;
        ev.type = PlayerEvent::Type::Error;
        ev.error = err;
        sink_.onEvent(ev);
    }
}

void RenderCoordinator::runRender(std::size_t frame, std::uint64_t generation) {
    if (generation != generation_.load(std::memory_order_acquire)) {
        return;  // stale request
    }
    if (!core_->isLoaded()) {
        return;
    }
    std::uint32_t* target = frameBuffer_.acquireRenderTarget();
    if (target == nullptr) {
        return;  // not sized yet
    }
    const FrameBuffer::Dimensions dims = frameBuffer_.dimensions();
    const bool ok = core_->renderFrame(frame, target, dims.width, dims.height,
                                       frameBuffer_.bytesPerLine(), /*preserveAspectRatio=*/true);
    if (!ok) {
        PlayerEvent ev;
        ev.type = PlayerEvent::Type::Error;
        ev.error = core_->lastError();
        ev.error.generation = generation;
        sink_.onEvent(ev);
        return;
    }
    // Drop the result if a newer generation arrived while we were rendering.
    if (generation != generation_.load(std::memory_order_acquire)) {
        return;
    }
    frameBuffer_.publish();
    sink_.onFramePublished(generation);
}

void RenderCoordinator::runSetColor(std::string keyPath, float r, float g, float b,
                                    std::uint64_t sourceEpoch) {
    if (sourceEpoch != sourceEpoch_.load(std::memory_order_acquire)) {
        return;  // the color keypath belongs to a source that has been replaced
    }
    core_->setColor(keyPath, r, g, b);
}

void RenderCoordinator::runSetOpacity(std::string keyPath, float opacity,
                                      std::uint64_t sourceEpoch) {
    if (sourceEpoch != sourceEpoch_.load(std::memory_order_acquire)) {
        return;  // the keypath belongs to a source that has been replaced
    }
    core_->setOpacity(keyPath, opacity);
}

void RenderCoordinator::runSetStrokeWidth(std::string keyPath, float width,
                                          std::uint64_t sourceEpoch) {
    if (sourceEpoch != sourceEpoch_.load(std::memory_order_acquire)) {
        return;  // the keypath belongs to a source that has been replaced
    }
    core_->setStrokeWidth(keyPath, width);
}

}  // namespace rnrlottie
