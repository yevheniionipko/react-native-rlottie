// Chunk 1.3 — FrameBuffer implementation.
#include "FrameBuffer.h"

#include <new>

#include "InputLimits.h"

namespace rnrlottie {

FrameBuffer::FrameBuffer(Limits limits) : limits_(limits) {}

bool FrameBuffer::resize(Dimensions dims, PlayerError& outError) {
    // Chunk 5.1: overflow-checked bounds-validation is shared with the core's
    // composition-size check (RlottiePlayerCore) via InputLimits::checkDimensions
    // so the two call sites cannot drift apart on what "invalid" means.
    const PlayerError dimErr = checkDimensions(dims.width, dims.height, limits_.maxPixels);
    if (dimErr) {
        outError = dimErr;
        return false;
    }
    const std::size_t pixels = dims.width * dims.height;

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

    // Chunk 7.1: only reached on a genuine (re)allocation, never on the
    // idempotent equal-dims early-return above or a rejected call.
    allocCount_.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t bytes = static_cast<std::uint64_t>(pixels) * 4u * 2u;  // both buffers
    std::uint64_t prevPeak = peakBytes_.load(std::memory_order_relaxed);
    while (bytes > prevPeak &&
          !peakBytes_.compare_exchange_weak(prevPeak, bytes, std::memory_order_relaxed)) {
    }
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
