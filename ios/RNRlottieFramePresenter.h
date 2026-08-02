// Chunk 2.2 — Core Graphics frame presenter.
//
// Turns a rnrlottie::FrameBuffer::FrontBuffer (raw ARGB32-premultiplied pixels
// owned by the C++ core, see cpp/FrameBuffer.h + cpp/PixelFormat.h) into a
// CGImageRef the view can assign to `layer.contents`, with NO per-frame pixel
// copy: CGDataProviderCreateWithData wraps the existing pixel pointer.
//
// Byte layout (cpp/PixelFormat.h, verified in docs/pixel-format-report.md):
// little-endian 32-bit word 0xAARRGGBB, premultiplied, top-down rows. This is
// exactly kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst — no
// channel swap needed on iOS (unlike Android).
//
// Data-provider lifetime vs. the buffer swap (see cpp/FrameBuffer.h's
// concurrency contract comment): in STEADY STATE (fixed dims), the render
// worker only ever writes into the *back* buffer; publish() atomically flips
// which of the two buffer slots is "front". A FrontBuffer snapshot handed to
// this presenter therefore points at memory the worker guarantees it will not
// write into again until at least one more publish() happens — i.e. until the
// view has already been asked (via a fresh onFramePresentable) to build the
// *next* CGImage and drop this one. So it is safe to reference that memory
// with zero copy for the lifetime of the CGImage, as long as the view never
// holds two "current" CGImages at once (it doesn't — each new frame replaces
// `layer.contents`, which releases the previous CGImage synchronously).
// A dims-changing resize() is a separate, structural case (not steady-state
// swapping) that RenderCoordinator (Chunk 1.5) serializes via a generation
// bump before it touches buffer memory; this presenter never calls
// readFront() itself outside of an onFramePresentable callback, so it never
// races that serialization.
#pragma once

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include "FrameBuffer.h"

NS_ASSUME_NONNULL_BEGIN

@interface RNRlottieFramePresenter : NSObject

- (instancetype)init NS_DESIGNATED_INITIALIZER;

// Builds a CGImageRef directly over frontBuffer.pixels (no copy). Returns
// NULL if frontBuffer.pixels is null or the dimensions are degenerate.
//
// CF_RETURNS_RETAINED: the caller owns +1 and must CGImageRelease it (or hand
// it to a property/API that takes ownership, e.g. assigning to
// `layer.contents` retains its own reference and the caller then releases
// its own +1).
- (nullable CGImageRef)newImageForFrontBuffer:
    (const rnrlottie::FrameBuffer::FrontBuffer &)frontBuffer CF_RETURNS_RETAINED;

@end

NS_ASSUME_NONNULL_END
