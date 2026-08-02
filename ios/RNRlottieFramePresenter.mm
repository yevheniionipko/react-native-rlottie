// Chunk 2.2 — Core Graphics frame presenter implementation.
#import "RNRlottieFramePresenter.h"

namespace {

// CGDataProviderCreateWithData's release callback fires when the CGImage (and
// its data provider) is fully released. We do NOT own the pixel memory — it
// belongs to the C++ FrameBuffer (cpp/FrameBuffer.h) and is reused as a
// render target by the worker, not freed here — so this is deliberately a
// no-op. Freeing it would be a double-free against FrameBuffer's own storage.
void ReleaseNoOp(void *info, const void *data, size_t size) {
    (void)info;
    (void)data;
    (void)size;
}

}  // namespace

@implementation RNRlottieFramePresenter {
    // Immutable, platform-independent color space (sRGB, per the Chunk 0.4
    // pixel-format report). Created once and reused for every frame — CGColorSpace
    // objects are relatively expensive to construct and are safe to share.
    CGColorSpaceRef _colorSpace;
}

- (instancetype)init {
    if ((self = [super init])) {
        _colorSpace = CGColorSpaceCreateDeviceRGB();
    }
    return self;
}

- (void)dealloc {
    if (_colorSpace) {
        CGColorSpaceRelease(_colorSpace);
        _colorSpace = NULL;
    }
}

- (nullable CGImageRef)newImageForFrontBuffer:
    (const rnrlottie::FrameBuffer::FrontBuffer &)frontBuffer {
    if (frontBuffer.pixels == nullptr || frontBuffer.width == 0 || frontBuffer.height == 0 ||
        frontBuffer.bytesPerLine == 0) {
        return NULL;
    }

    const size_t dataSize = frontBuffer.bytesPerLine * frontBuffer.height;

    // Zero-copy: wrap the existing pixel memory. `info` is unused (nothing to
    // free); see ReleaseNoOp above for why the release callback does nothing.
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        /*info=*/NULL, frontBuffer.pixels, dataSize, ReleaseNoOp);
    if (!provider) {
        return NULL;
    }

    // rlottie's surface (cpp/PixelFormat.h, verified in
    // docs/pixel-format-report.md) is little-endian ARGB32 premultiplied,
    // top-down — exactly this bitmapInfo, so no channel swap or row flip is
    // needed on iOS.
    const CGBitmapInfo bitmapInfo =
        (CGBitmapInfo)kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaPremultipliedFirst;

    CGImageRef image = CGImageCreate(
        /*width=*/frontBuffer.width,
        /*height=*/frontBuffer.height,
        /*bitsPerComponent=*/8,
        /*bitsPerPixel=*/32,
        /*bytesPerRow=*/frontBuffer.bytesPerLine,
        /*space=*/_colorSpace,
        /*bitmapInfo=*/bitmapInfo,
        /*provider=*/provider,
        /*decode=*/NULL,
        /*shouldInterpolate=*/true,
        /*intent=*/kCGRenderingIntentDefault);

    // CGImageCreate retains the provider itself; release our +1.
    CGDataProviderRelease(provider);

    return image;
}

@end
