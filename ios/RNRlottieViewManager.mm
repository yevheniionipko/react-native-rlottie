// Chunk 2.3 — RNRlottieViewManager: the legacy-bridge ViewManager.
//
// This is deliberately thin (CLAUDE.md: "Legacy bridge adapters ... Thin; no
// business logic") — the coalescing, prop-interpretation, and lifecycle logic
// live on RNRlottieView (Chunk 2.2/2.3, see RNRlottieView.h/.mm) or
// RNRlottiePlayer (Chunk 2.1). This file's job is purely wiring: exporting
// the component/props/events under the EXACT names in
// docs/bridge-contract.md (the frozen contract Android's RlottieViewManager
// mirrors), and dispatching the 8 numeric/string commands to the right view.
//
// ## Numeric command dispatch — how this actually works on RN <= 0.81
//
// `UIManager.dispatchViewManagerCommand(tag, commandId, args)` reaches
// `-[RCTUIManager dispatchViewManagerCommand:commandID:commandArgs:]`
// (RCTUIManager.mm), which resolves `commandID` one of two ways:
//
//   - NSString  -> `moduleData.methodsByName[commandID]`            (by name)
//   - NSNumber  -> `moduleData.methods[[commandID intValue]]`       (by index)
//
// `moduleData.methods` (RCTModuleData.mm's -calculateMethods) is built by
// walking this class's own `__rct_export__...` metaclass methods — i.e. the
// RCT_EXPORT_METHOD-annotated methods actually declared in THIS
// @implementation block — via `class_copyMethodList`, which for a single
// @implementation is populated in exact source declaration order. This is
// the same mechanism (and the same "commands are numbered by declaration
// order on iOS" rule) RN's old native-UI-component docs described for years.
// It means:
//
//   1. The array is 0-indexed, but the frozen contract's ids start at 1, so
//      declaration index 0 must be reserved (see -__reservedCommandSlot0:
//      below) purely to keep index N == command id N.
//   2. This class must export ONLY these 9 methods, in EXACTLY this order,
//      and nothing else via RCT_EXPORT_METHOD — an extra exported method
//      (before or interleaved) would silently shift every id after it.
//   3. If JS ever sends an int outside 0...8, `_methods[commandID]` is a
//      plain NSArray index and WILL raise NSRangeException — so the JS layer
//      (Chunk 4.1) must never emit an id outside the contract's 1...8.
//
// The STRING path is independent of all that fragility: `methodsByName` is
// keyed by each method's JS name (the text before the first `:`), so as long
// as the method names below match the contract's `name` column exactly
// ("play", "pause", ...), string dispatch works regardless of declaration
// order.
//
// Both paths deliver `(reactTag, ...args)` on the UIManager queue (see
// RCTViewManager.m's `-methodQueue` -> `RCTGetUIManagerQueue()`), NOT main —
// so every command below hops to main via `[uiManager addUIBlock:]` (the
// same pattern RCTUIManager's own `measure`/`scrollTo`-style commands use)
// before touching `view.player`, matching RNRlottieView's "every property
// setter and player call happens on MAIN" threading contract.
#import <React/RCTBridge.h>
#import <React/RCTConvert.h>
#import <React/RCTUIManager.h>
#import <React/RCTViewManager.h>

#import "RNRlottiePlayer.h"
#import "RNRlottieView.h"

NS_ASSUME_NONNULL_BEGIN

// Small hex-color parser for the `colorOverrides` prop
// (`#RRGGBB` / `#AARRGGBB` -> 0..1 float components). No alpha output: the
// underlying rnrlottie::RenderCoordinator::setColor(keyPath, r, g, b) API
// (Chunk 1.x) is RGB-only as of this chunk — an 8-digit hex's alpha byte is
// accepted (for forward/back compatibility with JS-side validation) but
// discarded here. Returns NO (and leaves out-params untouched) for anything
// that isn't a well-formed 6- or 8-digit `#`-prefixed hex string.
static BOOL RNRlottieParseHexColor(NSString *hex, float *outR, float *outG, float *outB) {
  if (![hex isKindOfClass:[NSString class]]) {
    return NO;
  }
  NSString *s = hex;
  if ([s hasPrefix:@"#"]) {
    s = [s substringFromIndex:1];
  }
  if (s.length != 6 && s.length != 8) {
    return NO;
  }
  unsigned int value = 0;
  NSScanner *scanner = [NSScanner scannerWithString:s];
  if (![scanner scanHexInt:&value] || !scanner.isAtEnd) {
    return NO;
  }
  // 8-digit form is AARRGGBB (matches the contract's stated format); the
  // trailing 6 bits/24 bits are always RRGGBB regardless of length.
  const unsigned int rgb = value & 0x00FFFFFFu;
  *outR = ((rgb >> 16) & 0xFF) / 255.0f;
  *outG = ((rgb >> 8) & 0xFF) / 255.0f;
  *outB = (rgb & 0xFF) / 255.0f;
  return YES;
}

@interface RNRlottieViewManager : RCTViewManager
@end

@implementation RNRlottieViewManager

// Explicit name: RCT_EXPORT_MODULE() with no argument would derive
// "RNRlottieView" from the Objective-C class name (stripping only the "RCT"
// prefix RN recognizes, not "RN"), which does NOT match the contract's
// required native component name "RlottieView" that
// requireNativeComponent<...>('RlottieView') resolves — see
// docs/bridge-contract.md's "Native component name" section.
RCT_EXPORT_MODULE(RlottieView)

+ (BOOL)requiresMainQueueSetup {
  return YES;  // -view below touches UIKit/CoreAnimation.
}

- (UIView *)view {
  return [[RNRlottieView alloc] init];
}

#pragma mark - Props: playback-shaping (coalesced on RNRlottieView; see its header)

RCT_EXPORT_VIEW_PROPERTY(loop, BOOL)
RCT_EXPORT_VIEW_PROPERTY(repeatCount, NSInteger)
RCT_EXPORT_VIEW_PROPERTY(speed, double)
RCT_EXPORT_VIEW_PROPERTY(startFrame, NSInteger)
RCT_EXPORT_VIEW_PROPERTY(endFrame, NSInteger)
RCT_EXPORT_VIEW_PROPERTY(autoPlay, BOOL)

#pragma mark - Props: other

RCT_EXPORT_VIEW_PROPERTY(progress, double)
RCT_EXPORT_VIEW_PROPERTY(resizeMode, NSString)
RCT_EXPORT_VIEW_PROPERTY(renderScale, double)
RCT_EXPORT_VIEW_PROPERTY(pauseWhenInactive, BOOL)
RCT_EXPORT_VIEW_PROPERTY(cacheStrategy, NSString)

// `source` needs conversion (it's a JS object, not a scalar/NSString), so it
// is the one prop that goes through RCT_CUSTOM_VIEW_PROPERTY rather than
// binding straight to a KVC setter. It still does no interpretation here —
// it just hands the raw dictionary to RNRlottieView's `pendingSource`, whose
// setter marks the view dirty; the actual {json}/{path}/INVALID_SOURCE
// decision happens in -[RNRlottieView flushPendingSourceIfNeeded] (coalesced
// with `cacheStrategy`, which may arrive before or after `source` in the same
// transaction).
RCT_CUSTOM_VIEW_PROPERTY(source, NSDictionary, RNRlottieView) {
  view.pendingSource = json ? [RCTConvert NSDictionary:json] : nil;
}

// `colorOverrides`: `[{keyPath, color}]`. Applied immediately (not coalesced
// — each entry is an independent, idempotent native call with no ordering
// dependency on any other prop, unlike `source`/`cacheStrategy`).
RCT_CUSTOM_VIEW_PROPERTY(colorOverrides, NSArray, RNRlottieView) {
  NSArray *overrides = json ? [RCTConvert NSArray:json] : nil;
  for (id item in overrides) {
    if (![item isKindOfClass:[NSDictionary class]]) {
      continue;
    }
    NSDictionary *entry = (NSDictionary *)item;
    NSString *keyPath = entry[@"keyPath"];
    NSString *color = entry[@"color"];
    if (![keyPath isKindOfClass:[NSString class]] || keyPath.length == 0) {
      continue;
    }
    float r, g, b;
    if (RNRlottieParseHexColor(color, &r, &g, &b)) {
      [view.player setColorForKeyPath:keyPath red:r green:g blue:b];
    }
  }
}

#pragma mark - Events (direct)

RCT_EXPORT_VIEW_PROPERTY(onAnimationLoaded, RCTDirectEventBlock)
RCT_EXPORT_VIEW_PROPERTY(onAnimationError, RCTDirectEventBlock)
RCT_EXPORT_VIEW_PROPERTY(onAnimationStart, RCTDirectEventBlock)
RCT_EXPORT_VIEW_PROPERTY(onAnimationPause, RCTDirectEventBlock)
RCT_EXPORT_VIEW_PROPERTY(onAnimationLoop, RCTDirectEventBlock)
RCT_EXPORT_VIEW_PROPERTY(onAnimationFinish, RCTDirectEventBlock)

#pragma mark - Commands

// Runs `block(view)` on main for `reactTag`, iff a live view is still
// registered under that tag. Per the contract: "Commands that arrive before
// the view has a live native handle are dropped silently, not queued and not
// an error" — a nil lookup (unmounted, or a stale/garbage tag) is exactly
// that silent drop, not an error path.
- (void)rnrlottie_withView:(NSNumber *)reactTag do:(void (^)(RNRlottieView *view))block {
  [self.bridge.uiManager
      addUIBlock:^(__unused RCTUIManager *uiManager, NSDictionary<NSNumber *, UIView *> *viewRegistry) {
        UIView *view = viewRegistry[reactTag];
        if (![view isKindOfClass:[RNRlottieView class]]) {
          return;
        }
        block((RNRlottieView *)view);
      }];
}

// Index 0 — reserved. See the file-header comment: the integer command path
// indexes directly into this class's exported-method array, and the
// contract's stable ids start at 1, so this placeholder exists solely to
// keep index N == command id N for the 8 real commands below. Never invoked
// by name (not in docs/bridge-contract.md) and never invoked by JS by index
// (JS only emits ids 1...8) — a genuine no-op.
RCT_EXPORT_METHOD(__reservedCommandSlot0 : (nonnull NSNumber *)reactTag) {
  (void)reactTag;
}

// id 1
RCT_EXPORT_METHOD(play
                  : (nonnull NSNumber *)reactTag startFrame
                  : (NSInteger)startFrame endFrame
                  : (NSInteger)endFrame) {
  [self rnrlottie_withView:reactTag
                         do:^(RNRlottieView *view) {
                           [view.player playFromFrame:startFrame toFrame:endFrame];
                         }];
}

// id 2
RCT_EXPORT_METHOD(pause : (nonnull NSNumber *)reactTag) {
  [self rnrlottie_withView:reactTag
                         do:^(RNRlottieView *view) {
                           [view.player pause];
                         }];
}

// id 3
RCT_EXPORT_METHOD(resume : (nonnull NSNumber *)reactTag) {
  [self rnrlottie_withView:reactTag
                         do:^(RNRlottieView *view) {
                           [view.player resume];
                         }];
}

// id 4
RCT_EXPORT_METHOD(stop : (nonnull NSNumber *)reactTag) {
  [self rnrlottie_withView:reactTag
                         do:^(RNRlottieView *view) {
                           [view.player stop];
                         }];
}

// id 5
RCT_EXPORT_METHOD(reset : (nonnull NSNumber *)reactTag) {
  [self rnrlottie_withView:reactTag
                         do:^(RNRlottieView *view) {
                           [view.player reset];
                         }];
}

// id 6
RCT_EXPORT_METHOD(seekToProgress : (nonnull NSNumber *)reactTag progress : (double)progress) {
  [self rnrlottie_withView:reactTag
                         do:^(RNRlottieView *view) {
                           [view.player seekToProgress:progress];
                         }];
}

// id 7
RCT_EXPORT_METHOD(seekToFrame : (nonnull NSNumber *)reactTag frame : (NSInteger)frame) {
  [self rnrlottie_withView:reactTag
                         do:^(RNRlottieView *view) {
                           [view.player seekToFrame:frame];
                         }];
}

// id 8
RCT_EXPORT_METHOD(setSpeed : (nonnull NSNumber *)reactTag speed : (double)speed) {
  [self rnrlottie_withView:reactTag
                         do:^(RNRlottieView *view) {
                           [view.player setSpeed:speed];
                         }];
}

@end

NS_ASSUME_NONNULL_END
