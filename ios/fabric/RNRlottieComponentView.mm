// Chunk 9.8 — the whole iOS Fabric adapter. See
// docs/new-architecture-design.md §3.6 for the contract this file implements
// (Option A: compose the existing RNRlottieView rather than reimplement it).
#ifdef RCT_NEW_ARCH_ENABLED

#import "RNRlottieComponentView.h"

#import <React/RCTConversions.h>

#import <react/renderer/components/RNRlottieSpec/ComponentDescriptors.h>
#import <react/renderer/components/RNRlottieSpec/EventEmitters.h>
#import <react/renderer/components/RNRlottieSpec/Props.h>

#import "RNRlottiePlayer.h"
#import "RNRlottieSourceResolver.h"
#import "RNRlottieView.h"

using namespace facebook::react;

// Same hex-color parsing rule as ios/RNRlottieViewManager.mm's
// RNRlottieParseHexColor, reimplemented over std::string since the Props
// struct's `color` field is std::string, not NSString. Kept in sync by
// inspection; both discard alpha and both reject anything that isn't a
// well-formed 6- or 8-digit `#`-prefixed hex string.
static BOOL RNRlottieFabricParseHexColor(const std::string &hex, float *outR, float *outG, float *outB) {
  NSString *s = RCTNSStringFromString(hex);
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
  const unsigned int rgb = value & 0x00FFFFFFu;
  *outR = ((rgb >> 16) & 0xFF) / 255.0f;
  *outG = ((rgb >> 8) & 0xFF) / 255.0f;
  *outB = (rgb & 0xFF) / 255.0f;
  return YES;
}

// §3.2.1: `source` is compared on `cacheKey`, never on `json`. If either
// side's cacheKey is empty, treat as changed (conservative — matches the
// Legacy "apply whatever arrives" behaviour); otherwise compare the four
// identity fields as strings.
static BOOL RNRlottieSourceChanged(const RlottieViewSourceStruct &oldSource,
                                    const RlottieViewSourceStruct &newSource) {
  if (oldSource.cacheKey.empty() || newSource.cacheKey.empty()) {
    return YES;
  }
  return oldSource.cacheKey != newSource.cacheKey || oldSource.path != newSource.path ||
      oldSource.uri != newSource.uri || oldSource.resourcePath != newSource.resourcePath;
}

// §3.2.2: hand-written comparators for the three override arrays — the
// generated structs have no `operator==` outside RN_SERIALIZABLE_STATE, so
// `std::vector`'s own `==` (which needs the element's `==`) does not compile.
static BOOL RNRlottieColorOverridesChanged(const std::vector<RlottieViewColorOverridesStruct> &a,
                                           const std::vector<RlottieViewColorOverridesStruct> &b) {
  if (a.size() != b.size()) {
    return YES;
  }
  for (size_t i = 0; i < a.size(); i++) {
    if (a[i].keyPath != b[i].keyPath || a[i].color != b[i].color) {
      return YES;
    }
  }
  return NO;
}

static BOOL RNRlottieOpacityOverridesChanged(const std::vector<RlottieViewOpacityOverridesStruct> &a,
                                             const std::vector<RlottieViewOpacityOverridesStruct> &b) {
  if (a.size() != b.size()) {
    return YES;
  }
  for (size_t i = 0; i < a.size(); i++) {
    if (a[i].keyPath != b[i].keyPath || a[i].opacity != b[i].opacity) {
      return YES;
    }
  }
  return NO;
}

static BOOL RNRlottieStrokeWidthOverridesChanged(const std::vector<RlottieViewStrokeWidthOverridesStruct> &a,
                                                 const std::vector<RlottieViewStrokeWidthOverridesStruct> &b) {
  if (a.size() != b.size()) {
    return YES;
  }
  for (size_t i = 0; i < a.size(); i++) {
    if (a[i].keyPath != b[i].keyPath || a[i].width != b[i].width) {
      return YES;
    }
  }
  return NO;
}

@interface RNRlottieComponentView ()
@end

@implementation RNRlottieComponentView {
  RNRlottieView *_view;
  std::shared_ptr<const RlottieViewEventEmitter> _rlottieEventEmitter;
}

+ (ComponentDescriptorProvider)componentDescriptorProvider {
  return concreteComponentDescriptorProvider<RlottieViewComponentDescriptor>();
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    static const auto defaultProps = std::make_shared<const RlottieViewProps>();
    _props = defaultProps;
    [self rnrlottie_createChildViewIfNeeded];
  }
  return self;
}

#pragma mark - Child view + event wiring

- (void)rnrlottie_createChildViewIfNeeded {
  if (_view) {
    return;
  }
  _view = [[RNRlottieView alloc] initWithFrame:self.bounds];
  [self addSubview:_view];

  __weak __typeof(self) weakSelf = self;

  _view.onAnimationLoaded = ^(NSDictionary *info) {
    __typeof(self) strongSelf = weakSelf;
    if (!strongSelf || !strongSelf->_rlottieEventEmitter) {
      return;
    }
    RlottieViewEventEmitter::OnAnimationLoaded payload{};
    payload.width = [info[@"width"] intValue];
    payload.height = [info[@"height"] intValue];
    payload.duration = [info[@"duration"] doubleValue];
    payload.frameRate = [info[@"frameRate"] doubleValue];
    payload.totalFrames = [info[@"totalFrames"] intValue];
    NSArray *markers = info[@"markers"];
    if ([markers isKindOfClass:[NSArray class]]) {
      for (id item in markers) {
        if (![item isKindOfClass:[NSDictionary class]]) {
          continue;
        }
        NSDictionary *marker = (NSDictionary *)item;
        RlottieViewEventEmitter::OnAnimationLoadedMarkers m{};
        NSString *name = marker[@"name"];
        m.name = [name isKindOfClass:[NSString class]] ? std::string(name.UTF8String) : std::string();
        m.startFrame = [marker[@"startFrame"] intValue];
        m.endFrame = [marker[@"endFrame"] intValue];
        payload.markers.push_back(std::move(m));
      }
    }
    strongSelf->_rlottieEventEmitter->onAnimationLoaded(payload);
  };

  _view.onAnimationError = ^(NSDictionary *info) {
    __typeof(self) strongSelf = weakSelf;
    if (!strongSelf || !strongSelf->_rlottieEventEmitter) {
      return;
    }
    RlottieViewEventEmitter::OnAnimationError payload{};
    NSString *code = info[@"code"];
    NSString *message = info[@"message"];
    payload.code = [code isKindOfClass:[NSString class]] ? std::string(code.UTF8String) : std::string();
    payload.message = [message isKindOfClass:[NSString class]] ? std::string(message.UTF8String) : std::string();
    strongSelf->_rlottieEventEmitter->onAnimationError(payload);
  };

  _view.onAnimationStart = ^(NSDictionary *info) {
    (void)info;
    __typeof(self) strongSelf = weakSelf;
    if (strongSelf && strongSelf->_rlottieEventEmitter) {
      strongSelf->_rlottieEventEmitter->onAnimationStart({});
    }
  };
  _view.onAnimationPause = ^(NSDictionary *info) {
    (void)info;
    __typeof(self) strongSelf = weakSelf;
    if (strongSelf && strongSelf->_rlottieEventEmitter) {
      strongSelf->_rlottieEventEmitter->onAnimationPause({});
    }
  };
  _view.onAnimationLoop = ^(NSDictionary *info) {
    (void)info;
    __typeof(self) strongSelf = weakSelf;
    if (strongSelf && strongSelf->_rlottieEventEmitter) {
      strongSelf->_rlottieEventEmitter->onAnimationLoop({});
    }
  };
  _view.onAnimationFinish = ^(NSDictionary *info) {
    (void)info;
    __typeof(self) strongSelf = weakSelf;
    if (strongSelf && strongSelf->_rlottieEventEmitter) {
      strongSelf->_rlottieEventEmitter->onAnimationFinish({});
    }
  };

  _view.onMetrics = ^(NSDictionary *info) {
    __typeof(self) strongSelf = weakSelf;
    if (!strongSelf || !strongSelf->_rlottieEventEmitter) {
      return;
    }
    RlottieViewEventEmitter::OnMetrics payload{};
    payload.parseMs = [info[@"parseMs"] doubleValue];
    payload.firstFrameMs = [info[@"firstFrameMs"] doubleValue];
    payload.renderP50Ms = [info[@"renderP50Ms"] doubleValue];
    payload.renderP95Ms = [info[@"renderP95Ms"] doubleValue];
    payload.renderP99Ms = [info[@"renderP99Ms"] doubleValue];
    payload.framesRendered = [info[@"framesRendered"] doubleValue];
    payload.framesDropped = [info[@"framesDropped"] doubleValue];
    payload.bufferAllocCount = [info[@"bufferAllocCount"] doubleValue];
    payload.peakBufferBytes = [info[@"peakBufferBytes"] doubleValue];
    payload.uiStallCount = [info[@"uiStallCount"] doubleValue];
    payload.uiStallMaxMs = [info[@"uiStallMaxMs"] doubleValue];
    strongSelf->_rlottieEventEmitter->onMetrics(payload);
  };
}

#pragma mark - Source

- (void)rnrlottie_applySource:(const RlottieViewSourceStruct &)source {
  NSMutableDictionary<NSString *, NSString *> *raw = [NSMutableDictionary dictionary];
  if (!source.json.empty()) {
    raw[@"json"] = RCTNSStringFromString(source.json);
  }
  if (!source.uri.empty()) {
    raw[@"uri"] = RCTNSStringFromString(source.uri);
  }
  if (!source.path.empty()) {
    raw[@"path"] = RCTNSStringFromString(source.path);
  }
  if (!source.cacheKey.empty()) {
    raw[@"cacheKey"] = RCTNSStringFromString(source.cacheKey);
  }
  if (!source.resourcePath.empty()) {
    raw[@"resourcePath"] = RCTNSStringFromString(source.resourcePath);
  }

  if (raw.count == 0) {
    _view.pendingSource = nil;
    return;
  }

  NSString *errorCode = nil;
  NSString *errorMessage = nil;
  NSDictionary *resolved = [RNRlottieSourceResolver resolveSource:raw
                                                          errorCode:&errorCode
                                                       errorMessage:&errorMessage];
  if (resolved) {
    _view.pendingSource = resolved;
  } else {
    _view.pendingSource = @{
      RNRlottieSourceResolverErrorKey : @{
        @"code" : errorCode ?: @"INVALID_SOURCE",
        @"message" : errorMessage ?: @"",
      },
    };
  }
}

#pragma mark - RCTComponentViewProtocol

- (void)updateProps:(const Props::Shared &)props oldProps:(const Props::Shared &)oldProps {
  [self rnrlottie_createChildViewIfNeeded];

  const auto &newProps = *std::static_pointer_cast<const RlottieViewProps>(props);
  static const RlottieViewProps defaultProps{};
  const auto &oldProps_ =
      oldProps ? *std::static_pointer_cast<const RlottieViewProps>(oldProps) : defaultProps;

  if (newProps.autoPlay != oldProps_.autoPlay) {
    _view.autoPlay = newProps.autoPlay;
  }
  if (newProps.loop != oldProps_.loop) {
    _view.loop = newProps.loop;
  }
  if (newProps.repeatCount != oldProps_.repeatCount) {
    _view.repeatCount = newProps.repeatCount;
  }
  if (newProps.speed != oldProps_.speed) {
    _view.speed = newProps.speed;
  }
  if (newProps.startFrame != oldProps_.startFrame) {
    _view.startFrame = newProps.startFrame;
  }
  if (newProps.endFrame != oldProps_.endFrame) {
    _view.endFrame = newProps.endFrame;
  }
  if (newProps.resizeMode != oldProps_.resizeMode) {
    _view.resizeMode = RCTNSStringFromString(newProps.resizeMode);
  }
  if (newProps.renderScale != oldProps_.renderScale) {
    _view.renderScale = newProps.renderScale;
  }
  if (newProps.pauseWhenInactive != oldProps_.pauseWhenInactive) {
    _view.pauseWhenInactive = newProps.pauseWhenInactive;
  }
  if (newProps.cacheStrategy != oldProps_.cacheStrategy) {
    _view.cacheStrategy = RCTNSStringFromString(newProps.cacheStrategy);
  }
  if (newProps.metricsEnabled != oldProps_.metricsEnabled) {
    _view.metricsEnabled = newProps.metricsEnabled;
  }
  // §3.2.4: seek only on a real change — there is no "unset" under Fabric.
  if (newProps.progress != oldProps_.progress) {
    _view.progress = newProps.progress;
  }

  if (RNRlottieSourceChanged(oldProps_.source, newProps.source)) {
    [self rnrlottie_applySource:newProps.source];
  }

  if (RNRlottieColorOverridesChanged(oldProps_.colorOverrides, newProps.colorOverrides)) {
    for (const auto &entry : newProps.colorOverrides) {
      if (entry.keyPath.empty()) {
        continue;
      }
      float r, g, b;
      if (RNRlottieFabricParseHexColor(entry.color, &r, &g, &b)) {
        [_view.player setColorForKeyPath:RCTNSStringFromString(entry.keyPath) red:r green:g blue:b];
      }
    }
  }
  if (RNRlottieOpacityOverridesChanged(oldProps_.opacityOverrides, newProps.opacityOverrides)) {
    for (const auto &entry : newProps.opacityOverrides) {
      if (entry.keyPath.empty()) {
        continue;
      }
      [_view.player setOpacityForKeyPath:RCTNSStringFromString(entry.keyPath) opacity:entry.opacity];
    }
  }
  if (RNRlottieStrokeWidthOverridesChanged(oldProps_.strokeWidthOverrides, newProps.strokeWidthOverrides)) {
    for (const auto &entry : newProps.strokeWidthOverrides) {
      if (entry.keyPath.empty()) {
        continue;
      }
      [_view.player setStrokeWidthForKeyPath:RCTNSStringFromString(entry.keyPath) width:entry.width];
    }
  }

  [super updateProps:props oldProps:oldProps];
}

- (void)updateEventEmitter:(const EventEmitter::Shared &)eventEmitter {
  _rlottieEventEmitter = std::static_pointer_cast<const RlottieViewEventEmitter>(eventEmitter);
  [super updateEventEmitter:eventEmitter];
}

- (void)updateLayoutMetrics:(const LayoutMetrics &)layoutMetrics
           oldLayoutMetrics:(const LayoutMetrics &)oldLayoutMetrics {
  [super updateLayoutMetrics:layoutMetrics oldLayoutMetrics:oldLayoutMetrics];
  // §3.7: reuses the child's own 100ms resize debounce via -layoutSubviews;
  // do NOT resize the surface synchronously here.
  _view.frame = self.bounds;
}

- (void)finalizeUpdates:(RNComponentViewUpdateMask)updateMask {
  [super finalizeUpdates:updateMask];
  if (updateMask & RNComponentViewUpdateMaskProps) {
    [_view didSetProps:@[]];
  }
}

- (void)handleCommand:(const NSString *)commandName args:(const NSArray *)args {
  RCTRlottieViewHandleCommand(self, commandName, args);
}

+ (BOOL)shouldBeRecycled {
  // §3.6: a pooled view would hold a live render worker + double buffers for
  // as long as it sits in the pool. Opting out keeps the Fabric lifecycle
  // isomorphic to Legacy: created on mount, destroyed on unmount.
  return NO;
}

- (void)prepareForRecycle {
  [super prepareForRecycle];
  [_view teardown];
  _view = nil;
  _rlottieEventEmitter.reset();
  static const auto defaultProps = std::make_shared<const RlottieViewProps>();
  _props = defaultProps;
  // A fresh child is created lazily on the next -updateProps:.
}

#pragma mark - RCTRlottieViewViewProtocol (imperative commands)
//
// Forwards straight to `_view.player`, exactly as RNRlottieViewManager.mm
// does for the Legacy path, minus the `addUIBlock:` hop — every Fabric
// callback here is already on main (§3.5).

- (void)play:(NSInteger)startFrame endFrame:(NSInteger)endFrame {
  [_view.player playFromFrame:startFrame toFrame:endFrame];
}

- (void)pause {
  [_view.player pause];
}

- (void)resume {
  [_view.player resume];
}

- (void)stop {
  [_view.player stop];
}

- (void)reset {
  [_view.player reset];
}

- (void)seekToProgress:(double)progress {
  [_view.player seekToProgress:progress];
}

- (void)seekToFrame:(NSInteger)frame {
  [_view.player seekToFrame:frame];
}

- (void)setPlaybackSpeed:(double)speed {
  [_view.player setSpeed:speed];
}

- (void)playMarker:(NSString *)name {
  [_view.player playMarker:name];
}

@end

#endif // RCT_NEW_ARCH_ENABLED
