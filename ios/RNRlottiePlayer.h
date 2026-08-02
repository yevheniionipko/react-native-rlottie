// Chunk 2.1 — iOS player adapter (Objective-C++).
//
// Owns the shared C++ RenderCoordinator (which has its own render worker thread)
// and a main-thread PlaybackController, and bridges the C++ FrameSink callbacks
// back to the main thread as Objective-C blocks. This is a platform adapter: it
// holds no UIView and does no CoreGraphics work — the view (Chunk 2.2) drives
// the display clock, reads -frontBuffer, and presents.
//
// Threading: every method here is called on the MAIN thread (RN bridge +
// CADisplayLink). The only cross-thread hop is the FrameSink, which marshals
// worker-thread callbacks back to main (see the .mm).
#import <Foundation/Foundation.h>

#import "FrameBuffer.h"

NS_ASSUME_NONNULL_BEGIN

/// onAnimationLoaded payload: width, height, duration, frameRate, totalFrames,
/// and markers (an array of {name, startFrame, endFrame}).
typedef void (^RNRlottieLoadedBlock)(NSDictionary *info);
/// onAnimationError payload: a stable machine-readable code + a message.
typedef void (^RNRlottieErrorBlock)(NSString *code, NSString *message);
/// start / pause / loop / finish lifecycle notifications.
typedef void (^RNRlottieLifecycleBlock)(void);
/// A frame was published; the view should read -frontBuffer and present it.
typedef void (^RNRlottieFrameBlock)(void);

@interface RNRlottiePlayer : NSObject

@property (nonatomic, copy, nullable) RNRlottieLoadedBlock onLoaded;
@property (nonatomic, copy, nullable) RNRlottieErrorBlock onError;
@property (nonatomic, copy, nullable) RNRlottieLifecycleBlock onStart;
@property (nonatomic, copy, nullable) RNRlottieLifecycleBlock onPause;
@property (nonatomic, copy, nullable) RNRlottieLifecycleBlock onLoop;
@property (nonatomic, copy, nullable) RNRlottieLifecycleBlock onFinish;
@property (nonatomic, copy, nullable) RNRlottieFrameBlock onFramePresentable;

- (instancetype)initWithMaxPixels:(size_t)maxPixels NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

// Source (already resolved by the platform resolver — raw JSON or a file path).
- (void)setSourceData:(NSString *)json
             cacheKey:(NSString *)cacheKey
         resourcePath:(nullable NSString *)resourcePath;
- (void)setSourceFile:(NSString *)path;

// Playback configuration. endFrame <= 0 means "last frame".
- (void)configureLoop:(BOOL)loop
          repeatCount:(int)repeatCount
                speed:(double)speed
           startFrame:(NSInteger)startFrame
             endFrame:(NSInteger)endFrame
             autoPlay:(BOOL)autoPlay;

// Imperative commands.
- (void)play;
- (void)pause;
- (void)resume;
- (void)stop;
- (void)reset;
- (void)seekToFrame:(NSInteger)frame;
- (void)seekToProgress:(double)progress;
- (void)setSpeed:(double)speed;

// Physical pixel dimensions of the render surface.
- (void)setSurfaceWidth:(size_t)width height:(size_t)height;

// Called each display tick with a monotonic timestamp in SECONDS. Advances the
// playback state machine, fires lifecycle blocks, and requests a render.
- (void)onDisplayTick:(double)monotonicSeconds;

// A consistent snapshot of the presented buffer for the view to build a CGImage.
// pixels == nullptr when nothing is available (or after teardown).
- (rnrlottie::FrameBuffer::FrontBuffer)frontBuffer;

// Idempotent. Joins the worker (no block fires afterwards) and releases native
// resources. Also invoked from -dealloc.
- (void)teardown;

@end

NS_ASSUME_NONNULL_END
