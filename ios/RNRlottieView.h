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

// --- Chunk 2.3 additions: view-level props the ViewManager binds directly ---
//
// These are plain KVC-settable properties so RCT_EXPORT_VIEW_PROPERTY can
// bind straight to them (no business logic in RNRlottieViewManager, per
// CLAUDE.md's "thin adapter" rule) — the logic that would otherwise live in
// the manager lives in these setters instead, matching how `renderScale`
// (Chunk 2.2, above) already works.

// `resizeMode` JS prop: "contain" (default) | "cover" | "stretch" | "center".
// Maps to `layer.contentsGravity`. Unrecognized values fall back to
// "contain" (matches the contract's stated default).
@property (nonatomic, copy) NSString *resizeMode;

// `progress` JS prop (0..1): an immediate seek, not part of the coalesced
// configure() call below — matches docs/bridge-contract.md, which lists it
// separately from the playback-shaping props.
@property (nonatomic, assign) double progress;

// `pauseWhenInactive` JS prop, default YES. Gates ONLY the app
// background/foreground lifecycle pause below (-handleAppDidEnterBackground /
// -handleAppWillEnterForeground); window attach/detach pause (-didMoveToWindow)
// is a hard rendering constraint (no window, no display link) and stays
// unconditional, per the existing Chunk 2.2 behavior.
@property (nonatomic, assign) BOOL pauseWhenInactive;

// `cacheStrategy` JS prop: "none" | "model" (default). Stored, not applied
// immediately — read at the moment a pending `source` is flushed (see
// -setPendingSource: below) so the two props combine correctly regardless of
// which one JS sets first within a prop transaction.
@property (nonatomic, copy) NSString *cacheStrategy;

// Playback-shaping props the contract requires to be coalesced into ONE
// native `configure(...)` call per prop transaction (`loop`, `repeatCount`,
// `speed`, `startFrame`, `endFrame`, `autoPlay`). Each setter below just
// stores the value and marks the view dirty; -didSetProps: (RN's "all prop
// setters for this transaction have run" hook — see UIView+React.h /
// RCTUIManager.mm's `_dispatchPropsDidChangeEvents`) flushes exactly once.
// RCT_EXPORT_VIEW_PROPERTY binds directly to these, so the ViewManager itself
// contains none of this coalescing logic.
@property (nonatomic, assign) BOOL loop;
@property (nonatomic, assign) NSInteger repeatCount;
@property (nonatomic, assign) double speed;
@property (nonatomic, assign) NSInteger startFrame;
@property (nonatomic, assign) NSInteger endFrame;
@property (nonatomic, assign) BOOL autoPlay;

// `source` JS prop, already resolved to `{json,cacheKey,resourcePath}` or
// `{path}` by RNRlottieViewManager (full URI/asset resolution is Chunk 2.4 —
// see the manager). Stored as a pending dictionary and flushed from
// -didSetProps: together with `cacheStrategy` above. `nil` clears/no-ops.
@property (nonatomic, copy, nullable) NSDictionary *pendingSource;

// Called once per prop transaction by RN after every RCT_EXPORT_VIEW_PROPERTY
// setter above has run (see RCTComponent.h / UIView+React.h). Flushes the
// coalesced configure() call and, if `pendingSource` was set, the source.
- (void)didSetProps:(NSArray<NSString *> *)changedProps;

// Idempotent. Invalidates the display link, removes notification observers,
// and tears down the player so no callback (frame or event) can fire
// afterwards. Also invoked from -dealloc — safe to call early (e.g. on
// unmount) so cleanup isn't deferred to ARC's dealloc timing.
- (void)teardown;

@end

NS_ASSUME_NONNULL_END
