// Chunk 2.4 — RNRlottieModule: the global (process-wide) native module.
//
// Exposes ONLY process-global rlottie operations (plan §15 /
// docs/bridge-contract.md "Global module"): the shared model-cache size and
// the vendored rlottie version. This module NEVER routes per-view playback —
// that stays on RNRlottieViewManager / RNRlottiePlayer (Chunk 2.1-2.3). Every
// method here is a thin, one-line forward into
// rnrlottie::ModelCacheController (cpp/ModelCacheController.h, itself
// process-global and internally serialized) or the compile-time
// rnrlottie::kRlottieCommit constant — no business logic lives here, per
// CLAUDE.md's "thin adapter" rule.
#import <React/RCTBridgeModule.h>

#include "ModelCacheController.h"
#include "RlottieVersion.h"

@interface RNRlottieModule : NSObject <RCTBridgeModule>
@end

@implementation RNRlottieModule

RCT_EXPORT_MODULE(RlottieModule)

// ModelCacheController is a process-global C++ singleton with its own
// internal serialization (see its header) — this module touches no UIKit or
// Core Animation state, so it needs neither the main queue nor a dedicated
// serial one. Falls back to RCTBridgeModule's default (a private background
// queue), same as most non-UI native modules.
+ (BOOL)requiresMainQueueSetup {
  return NO;
}

// All three methods are Promise-based, matching Android's RlottieModule
// exactly. A fire-and-forget `configure`/`clearModelCache` here would make
// `await Rlottie.configure(...)` surface failures on Android but silently
// resolve on iOS — a platform-conditional error contract, which is precisely
// what docs/bridge-contract.md exists to prevent.

// `configure({modelCacheSize})` -> ModelCacheController::setModelCacheSize.
// A missing/non-numeric/negative `modelCacheSize` is a silent no-op rather
// than clamped to 0 — clamping to 0 would surprise-disable caching entirely
// on a bad call. Callers can always read back what actually took effect via
// -getNativeVersion's `modelCacheSize`.
RCT_REMAP_METHOD(configure,
                  configureWithOptions
                  : (NSDictionary *)options resolver
                  : (RCTPromiseResolveBlock)resolve rejecter
                  : (RCTPromiseRejectBlock)reject) {
  (void)reject;  // Nothing here can fail.
  id rawSize = options[@"modelCacheSize"];
  if ([rawSize isKindOfClass:[NSNumber class]]) {
    NSInteger size = [(NSNumber *)rawSize integerValue];
    if (size >= 0) {
      rnrlottie::ModelCacheController::setModelCacheSize(static_cast<std::size_t>(size));
    }
  }
  resolve(nil);
}

// `clearModelCache()` -> ModelCacheController::clearModelCache. Flushes the
// cache while preserving the configured size (see that header).
RCT_REMAP_METHOD(clearModelCache,
                  clearModelCacheWithResolver
                  : (RCTPromiseResolveBlock)resolve rejecter
                  : (RCTPromiseRejectBlock)reject) {
  (void)reject;  // Nothing here can fail.
  rnrlottie::ModelCacheController::clearModelCache();
  resolve(nil);
}

// `getNativeVersion()` -> {rlottieCommit, modelCacheSize}. Promise-based (RN's
// idiomatic shape for a native-module call that returns a value) even though
// the work itself is synchronous and cannot fail — resolves immediately.
RCT_REMAP_METHOD(getNativeVersion,
                  getNativeVersionWithResolver
                  : (RCTPromiseResolveBlock)resolve rejecter
                  : (RCTPromiseRejectBlock)reject) {
  (void)reject;  // Nothing here can fail.
  resolve(@{
    @"rlottieCommit" : [NSString stringWithUTF8String:rnrlottie::kRlottieCommit],
    @"modelCacheSize" : @(static_cast<NSInteger>(rnrlottie::ModelCacheController::currentSize())),
  });
}

@end
