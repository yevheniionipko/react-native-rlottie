// Chunk 2.2 — view implementation.
#import "RNRlottieView.h"

#import <UIKit/UIKit.h>

#include "InputLimits.h"

#import "RNRlottieFramePresenter.h"

// Physical-pixel per-axis clamp applied before the surface size ever reaches
// the player. This is a cheap, early defensive bound (avoids handing the
// player pathological w/h from a bogus layout pass); the *authoritative*
// byte-budget clamp is FrameBuffer's maxPixels check (cpp/FrameBuffer.h),
// which reports InvalidDimensions via onAnimationError if exceeded.
static const CGFloat kRNRlottieMaxSurfaceDimension = 4096.0;

// Debounce window for layout-driven resizes (plan §7: "debounce buffer
// recreation" on a resize storm, e.g. during an interactive layout animation
// or a flex-layout pass that fires layoutSubviews repeatedly for one visual
// change). Coalesces to the LAST size requested within the window.
static const NSTimeInterval kRNRlottieResizeDebounceInterval = 0.1;

// A CADisplayLink retains its target strongly for as long as it is not
// invalidated. If RNRlottieView were the target directly, a running display
// link would keep the view alive even after RN drops all its own references
// (the retain-cycle risk called out in the plan) — the view (and its player,
// buffers, etc.) would leak for the lifetime of the app, since nothing but
// the view itself normally calls -invalidate.
//
// This proxy breaks the cycle: CADisplayLink retains the PROXY, and the proxy
// holds only a __weak reference to the view. Once the view is otherwise
// unretained, it deallocs on schedule (running -teardown/-dealloc, which
// invalidates the link); until then, ticks are forwarded transparently.
//
// (Objective-C @interface/@implementation must live at file/global scope, so
// this is not wrapped in an anonymous C++ namespace — it is still translation-
// unit-private simply by not being declared in any header.)
@interface RNRlottieDisplayLinkProxy : NSProxy
- (instancetype)initWithTarget:(id)target;
@end

@implementation RNRlottieDisplayLinkProxy {
    __weak id _target;
}

- (instancetype)initWithTarget:(id)target {
    // NSProxy has no -init to call super to; nothing else to set up.
    _target = target;
    return self;
}

- (id)forwardingTargetForSelector:(SEL)aSelector {
    (void)aSelector;
    // nil target (view deallocated) => the message is silently dropped: per
    // NSProxy's contract, returning nil here falls through to
    // -methodSignatureForSelector:/-forwardInvocation:, which we also
    // implement defensively below in case some selector reaches them anyway.
    return _target;
}

- (NSMethodSignature *)methodSignatureForSelector:(SEL)aSelector {
    NSMethodSignature *sig = [_target methodSignatureForSelector:aSelector];
    if (sig) {
        return sig;
    }
    // Target is gone; hand back a harmless signature so -forwardInvocation:
    // below can no-op instead of NSProxy raising doesNotRecognizeSelector:.
    return [NSObject instanceMethodSignatureForSelector:@selector(self)];
}

- (void)forwardInvocation:(NSInvocation *)invocation {
    (void)invocation;
    // Target already gone by the time this actually runs (the common path is
    // -forwardingTargetForSelector: above); no-op rather than crash.
}

@end

@implementation RNRlottieView {
    RNRlottiePlayer *_player;
    RNRlottieFramePresenter *_presenter;
    CADisplayLink *_displayLink;

    // Debounced/applied physical pixel surface size.
    size_t _pendingSurfaceWidth;
    size_t _pendingSurfaceHeight;
    size_t _appliedSurfaceWidth;
    size_t _appliedSurfaceHeight;
    BOOL _hasPendingResize;

    // Lifecycle bookkeeping (plan §9: "resume according to previous state").
    // We only ever resume via -teardown-safe player calls if WE were the one
    // that paused for a lifecycle reason (background / off-window) while
    // playback was actually in motion — never overriding an explicit pause
    // the consumer/JS issued directly through `player`.
    BOOL _isPlayingIntent;       // tracks the player's own onStart/onPause/onFinish
    BOOL _pausedByLifecycle;     // we (not JS) paused it; safe for us to resume it
    BOOL _observersInstalled;
}

@synthesize player = _player;

#pragma mark - Init / dealloc

- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _renderScale = 1.0;
        _presenter = [[RNRlottieFramePresenter alloc] init];
        _player = [[RNRlottiePlayer alloc] initWithMaxPixels:rnrlottie::InputLimits{}.maxPixels];

        self.layer.contentsScale = [UIScreen mainScreen].scale;
        // We manage layer.contents pixel-for-pixel ourselves; disable any
        // implicit CA fade/animation on assignment so a new frame appears
        // immediately (a crossfade would blur the still-image tests and
        // fight the display-link cadence).
        [self.layer removeAllAnimations];

        [self wirePlayerCallbacks];
        [self installAppLifecycleObserversIfNeeded];
    }
    return self;
}

- (instancetype)initWithCoder:(NSCoder *)coder {
    if ((self = [super initWithCoder:coder])) {
        _renderScale = 1.0;
        _presenter = [[RNRlottieFramePresenter alloc] init];
        _player = [[RNRlottiePlayer alloc] initWithMaxPixels:rnrlottie::InputLimits{}.maxPixels];
        self.layer.contentsScale = [UIScreen mainScreen].scale;
        [self wirePlayerCallbacks];
        [self installAppLifecycleObserversIfNeeded];
    }
    return self;
}

- (void)dealloc {
    [self teardown];
}

#pragma mark - Player callback wiring

- (void)wirePlayerCallbacks {
    __weak RNRlottieView *weakSelf = self;

    _player.onLoaded = ^(NSDictionary *info) {
        RNRlottieView *strongSelf = weakSelf;
        if (strongSelf.onAnimationLoaded) {
            strongSelf.onAnimationLoaded(info);
        }
    };
    _player.onError = ^(NSString *code, NSString *message) {
        RNRlottieView *strongSelf = weakSelf;
        if (strongSelf.onAnimationError) {
            strongSelf.onAnimationError(@{@"code" : code, @"message" : message});
        }
    };
    _player.onStart = ^{
        RNRlottieView *strongSelf = weakSelf;
        if (!strongSelf) return;
        strongSelf->_isPlayingIntent = YES;
        if (strongSelf.onAnimationStart) {
            strongSelf.onAnimationStart(@{});
        }
    };
    _player.onPause = ^{
        RNRlottieView *strongSelf = weakSelf;
        if (!strongSelf) return;
        strongSelf->_isPlayingIntent = NO;
        if (strongSelf.onAnimationPause) {
            strongSelf.onAnimationPause(@{});
        }
    };
    _player.onLoop = ^{
        RNRlottieView *strongSelf = weakSelf;
        if (strongSelf.onAnimationLoop) {
            strongSelf.onAnimationLoop(@{});
        }
    };
    _player.onFinish = ^{
        RNRlottieView *strongSelf = weakSelf;
        if (!strongSelf) return;
        strongSelf->_isPlayingIntent = NO;
        if (strongSelf.onAnimationFinish) {
            strongSelf.onAnimationFinish(@{});
        }
    };
    _player.onFramePresentable = ^{
        [weakSelf presentCurrentFrame];
    };
}

#pragma mark - Presentation

- (void)presentCurrentFrame {
    // Called on [main] synchronously from the player's SinkBridge marshaling
    // (RNRlottiePlayer.mm already hopped worker->main before invoking this
    // block), so it is safe to touch the layer directly here.
    const rnrlottie::FrameBuffer::FrontBuffer front = [_player frontBuffer];
    CGImageRef image = [_presenter newImageForFrontBuffer:front];
    if (!image) {
        // Nothing presentable yet (e.g. teardown raced us, or dims not ready)
        // — per plan §7, never blank the layer; just keep the last frame.
        return;
    }
    // Assigning to `contents` retains `image` for CALayer's own +1; release
    // our +1 immediately after. The PREVIOUS CGImage (if any) is released by
    // this same assignment, which is what makes the zero-copy data provider
    // safe to reuse across frames — see RNRlottieFramePresenter.h's header
    // comment on data-provider lifetime vs. the buffer swap.
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    self.layer.contents = (__bridge id)image;
    [CATransaction commit];
    CGImageRelease(image);
}

#pragma mark - Display link

- (void)startDisplayLinkIfNeeded {
    if (_displayLink || !self.window) {
        return;
    }
    RNRlottieDisplayLinkProxy *proxy = [[RNRlottieDisplayLinkProxy alloc] initWithTarget:self];
    _displayLink = [CADisplayLink displayLinkWithTarget:proxy selector:@selector(onDisplayLinkTick:)];
    [_displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)stopDisplayLink {
    [_displayLink invalidate];
    _displayLink = nil;
}

// CADisplayLink's actual selector target (kept distinct from the public
// -onDisplayTick: so the latter has a clean, testable `(double)` signature
// per the plan's contract rather than a `(CADisplayLink *)` one).
- (void)onDisplayLinkTick:(CADisplayLink *)link {
    [self onDisplayTick:link.timestamp];
}

// Tick → PlaybackController.advance → coordinator.requestFrame, per the
// Chunk 2.1 contract. `monotonicSeconds` is CADisplayLink's own `timestamp`,
// which is already a mach-time-derived monotonic seconds value — exactly
// what PlaybackController expects (cpp/PlaybackController.h).
- (void)onDisplayTick:(double)monotonicSeconds {
    [_player onDisplayTick:monotonicSeconds];
}

#pragma mark - Layout / resize

- (void)layoutSubviews {
    [super layoutSubviews];
    [self scheduleSurfaceResizeForBounds:self.bounds];
}

- (void)setRenderScale:(CGFloat)renderScale {
    _renderScale = renderScale > 0 ? renderScale : 1.0;
    [self scheduleSurfaceResizeForBounds:self.bounds];
}

- (void)scheduleSurfaceResizeForBounds:(CGRect)bounds {
    if (CGRectIsEmpty(bounds)) {
        return;
    }
    const CGFloat screenScale = self.window.screen.scale ?: [UIScreen mainScreen].scale;
    const CGFloat renderScale = _renderScale > 0 ? _renderScale : 1.0;

    double pixelWidth = (double)bounds.size.width * screenScale * renderScale;
    double pixelHeight = (double)bounds.size.height * screenScale * renderScale;

    // Defensive clamp per axis (see kRNRlottieMaxSurfaceDimension above);
    // FrameBuffer enforces the real byte-budget clamp.
    pixelWidth = MIN(MAX(pixelWidth, 0.0), (double)kRNRlottieMaxSurfaceDimension);
    pixelHeight = MIN(MAX(pixelHeight, 0.0), (double)kRNRlottieMaxSurfaceDimension);

    const size_t w = (size_t)llround(pixelWidth);
    const size_t h = (size_t)llround(pixelHeight);
    if (w == 0 || h == 0) {
        return;
    }
    if (w == _pendingSurfaceWidth && h == _pendingSurfaceHeight && _hasPendingResize) {
        return;  // identical request already debouncing — don't restart the timer
    }
    if (!_hasPendingResize && w == _appliedSurfaceWidth && h == _appliedSurfaceHeight) {
        return;  // no-op: matches what's already applied
    }

    _pendingSurfaceWidth = w;
    _pendingSurfaceHeight = h;
    _hasPendingResize = YES;

    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                              selector:@selector(applyPendingSurfaceResize)
                                                object:nil];
    [self performSelector:@selector(applyPendingSurfaceResize)
                withObject:nil
                afterDelay:kRNRlottieResizeDebounceInterval];
}

- (void)applyPendingSurfaceResize {
    if (!_hasPendingResize) {
        return;
    }
    _hasPendingResize = NO;
    if (_pendingSurfaceWidth == _appliedSurfaceWidth &&
        _pendingSurfaceHeight == _appliedSurfaceHeight) {
        return;
    }
    _appliedSurfaceWidth = _pendingSurfaceWidth;
    _appliedSurfaceHeight = _pendingSurfaceHeight;
    // The player/coordinator bumps the generation and resizes on the render
    // worker; per plan §7 we deliberately do NOT touch layer.contents here —
    // the last valid frame stays up until -presentCurrentFrame is next
    // called for the new dimensions (onFramePresentable), matching
    // FrameBuffer's "front invalidated until next publish()" contract.
    [_player setSurfaceWidth:_appliedSurfaceWidth height:_appliedSurfaceHeight];
}

#pragma mark - Window / app lifecycle

- (void)didMoveToWindow {
    [super didMoveToWindow];
    if (self.window) {
        [self startDisplayLinkIfNeeded];
        [self resumeFromLifecyclePauseIfNeeded];
        // Re-run layout in case bounds/scale changed while off-window (e.g.
        // recycled into a different cell) without an intervening layout pass.
        [self scheduleSurfaceResizeForBounds:self.bounds];
    } else {
        [self stopDisplayLink];
        [self pauseForLifecycleReason];
    }
}

- (void)installAppLifecycleObserversIfNeeded {
    if (_observersInstalled) {
        return;
    }
    _observersInstalled = YES;
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserver:self
           selector:@selector(handleAppDidEnterBackground)
               name:UIApplicationDidEnterBackgroundNotification
             object:nil];
    [nc addObserver:self
           selector:@selector(handleAppWillEnterForeground)
               name:UIApplicationWillEnterForegroundNotification
             object:nil];
}

- (void)handleAppDidEnterBackground {
    [self stopDisplayLink];
    [self pauseForLifecycleReason];
}

- (void)handleAppWillEnterForeground {
    if (self.window) {
        [self startDisplayLinkIfNeeded];
    }
    [self resumeFromLifecyclePauseIfNeeded];
}

// Pauses playback for a lifecycle reason (backgrounding / leaving the
// window), but ONLY if it was actually in motion, and remembers that WE did
// it — so foreground/re-attach resumes it, without ever overriding an
// explicit pause the consumer issued directly via `player`.
- (void)pauseForLifecycleReason {
    if (_isPlayingIntent && !_pausedByLifecycle) {
        _pausedByLifecycle = YES;
        [_player pause];
    }
}

- (void)resumeFromLifecyclePauseIfNeeded {
    if (_pausedByLifecycle) {
        _pausedByLifecycle = NO;
        [_player resume];
    }
}

#pragma mark - Teardown

- (void)teardown {
    [self stopDisplayLink];
    if (_observersInstalled) {
        [[NSNotificationCenter defaultCenter] removeObserver:self];
        _observersInstalled = NO;
    }
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
    // Joins the render worker and nils all of the player's own callback
    // blocks first, so nothing queued on main can reach -presentCurrentFrame
    // (or any event block) after this point.
    [_player teardown];
    self.onAnimationLoaded = nil;
    self.onAnimationError = nil;
    self.onAnimationStart = nil;
    self.onAnimationPause = nil;
    self.onAnimationLoop = nil;
    self.onAnimationFinish = nil;
}

@end
