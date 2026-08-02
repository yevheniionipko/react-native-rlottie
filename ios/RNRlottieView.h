// Chunk 2.2 — the UIView that drives the display clock and presents frames.
//
// Owns an RNRlottiePlayer (Chunk 2.1) and a CADisplayLink. Every display tick
// is forwarded to -onDisplayTick:, which drives -[RNRlottiePlayer
// onDisplayTick:]; when the player says a frame is presentable, this view
// reads -frontBuffer, builds a CGImage via RNRlottieFramePresenter (no
// per-frame pixel copy — see that header), and assigns it to layer.contents.
//
// This view holds no React Native imports beyond RCTDirectEventBlock's plain
// block typedef (from RCTComponent.h — Foundation/CoreGraphics only, no bridge
// dependency) so Chunk 2.3's RCTViewManager can bind RCT_EXPORT_VIEW_PROPERTY
// directly to the properties below. All prop/command plumbing (source,
// loop/speed/etc. config, resizeMode, imperative commands) is Chunk 2.3's job;
// it reaches the player through the `player` accessor below.
//
// Threading: -onDisplayTick: and all property setters run on MAIN (CADisplayLink
// callbacks and the RN bridge are both main-thread in the legacy architecture).
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#import <React/RCTComponent.h>  // RCTDirectEventBlock only — no bridge dependency.

#import "RNRlottiePlayer.h"

NS_ASSUME_NONNULL_BEGIN

@interface RNRlottieView : UIView

// The owned player adapter. Chunk 2.3 (RNRlottieViewManager) uses this to
// forward prop updates (source, loop/speed/frame range/etc.) and imperative
// commands (play/pause/seek/…). Never nil for the lifetime of the view;
// becomes inert (all calls are no-ops) after -teardown / dealloc.
@property (nonatomic, strong, readonly) RNRlottiePlayer *player;

// Direct events. Chunk 2.3 binds these with RCT_EXPORT_VIEW_PROPERTY(..,
// RCTDirectEventBlock) to the corresponding JS prop. Never fires after
// -teardown / dealloc (the underlying player blocks are nilled out first).
//
// onAnimationLoaded body: the metadata dictionary the player already builds
//   (width/height/duration/frameRate/totalFrames/markers).
// onAnimationError body: {"code": NSString, "message": NSString}.
// onAnimationStart/Pause/Loop/Finish body: {} (no payload; presence is the
//   signal — matches the player's lifecycle blocks, which carry none either).
@property (nonatomic, copy, nullable) RCTDirectEventBlock onAnimationLoaded;
@property (nonatomic, copy, nullable) RCTDirectEventBlock onAnimationError;
@property (nonatomic, copy, nullable) RCTDirectEventBlock onAnimationStart;
@property (nonatomic, copy, nullable) RCTDirectEventBlock onAnimationPause;
@property (nonatomic, copy, nullable) RCTDirectEventBlock onAnimationLoop;
@property (nonatomic, copy, nullable) RCTDirectEventBlock onAnimationFinish;

// Multiplier applied on top of `layoutSize × screenScale` when computing the
// physical pixel render surface (plan §7: pixelW = layoutW × screenScale ×
// renderScale). Defaults to 1.0. Chunk 2.3 sets this from the `renderScale`
// JS prop; changing it triggers the same debounced resize path as a layout
// change.
@property (nonatomic, assign) CGFloat renderScale;

// Idempotent. Invalidates the display link, removes notification observers,
// and tears down the player so no callback (frame or event) can fire
// afterwards. Also invoked from -dealloc — safe to call early (e.g. on
// unmount) so cleanup isn't deferred to ARC's dealloc timing.
- (void)teardown;

@end

NS_ASSUME_NONNULL_END
