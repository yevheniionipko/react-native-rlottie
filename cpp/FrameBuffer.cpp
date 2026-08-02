// Chunk 1.3 — FrameBuffer implementation.
#include "FrameBuffer.h"

#include <limits>
#include <new>

namespace rnrlottie {

FrameBuffer::FrameBuffer(Limits limits) : limits_(limits) {}

bool FrameBuffer::resize(Dimensions dims, PlayerError& outError) {
    if (dims.width == 0 || dims.height == 0) {
        outError = PlayerError{PlayerErrorCode::InvalidDimensions,
                               "zero width or height", 0};
        return false;
    }
    constexpr std::size_t kMaxAxis = std::numeric_limits<std::uint32_t>::max();
    if (dims.width > kMaxAxis || dims.height > kMaxAxis) {
        outError = PlayerError{PlayerErrorCode::InvalidDimensions,
                               "dimension exceeds 32-bit range", 0};
        return false;
    }
    if (dims.width > std::numeric_limits<std::size_t>::max() / dims.height) {
        outError = PlayerError{PlayerErrorCode::InvalidDimensions,
                               "pixel count overflow", 0};
        return false;
    }
    const std::size_t pixels = dims.width * dims.height;
    if (pixels > limits_.maxPixels) {
        outError = PlayerError{PlayerErrorCode::InvalidDimensions,
                               "pixel count exceeds configured maximum", 0};
        return false;
    }

    // Idempotent: equal dims reuse the existing allocation and keep the front.
    if (dims.width == dims_.width && dims.height == dims_.height &&
        buffers_[0].size() == pixels) {
        return true;
    }

    try {
        // Value-initialize to 0 => transparent premultiplied ARGB.
        buffers_[0].assign(pixels, 0u);
        buffers_[1].assign(pixels, 0u);
    } catch (const std::bad_alloc&) {
        outError = PlayerError{PlayerErrorCode::AllocationFailed,
                               "failed to allocate frame buffers", 0};
        return false;
    }

    dims_ = dims;
    backIndex_ = 0;
    frontIndex_.store(-1, std::memory_order_release);  // front invalid until publish
    return true;
}

std::uint32_t* FrameBuffer::acquireRenderTarget() {
    if (!isSized()) {
        return nullptr;
    }
    return buffers_[static_cast<std::size_t>(backIndex_)].data();
}

FrameBuffer::Dimensions FrameBuffer::dimensions() const { return dims_; }

std::size_t FrameBuffer::bytesPerLine() const { return dims_.width * 4; }

void FrameBuffer::publish() {
    if (!isSized()) {
        return;
    }
    // Release so the rendered pixels are visible to a [main] readFront() that
    // observes this index with acquire.
    frontIndex_.store(backIndex_, std::memory_order_release);
    backIndex_ = 1 - backIndex_;  // next render targets the other buffer
}

FrameBuffer::FrontBuffer FrameBuffer::readFront() const {
    const int f = frontIndex_.load(std::memory_order_acquire);
    if (f < 0 || dims_.width == 0 || dims_.height == 0) {
        return FrontBuffer{};
    }
    // dims_ is stable during the steady-state concurrent phase (see header
    // contract); resize() never overlaps readFront().
    return FrontBuffer{buffers_[static_cast<std::size_t>(f)].data(), dims_.width,
                       dims_.height, dims_.width * 4};
}

bool FrameBuffer::isSized() const { return dims_.width != 0 && dims_.height != 0; }

}  // namespace rnrlottie
