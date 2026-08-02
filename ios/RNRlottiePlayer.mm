// Chunk 2.1 — iOS player adapter implementation.
#import "RNRlottiePlayer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#import "AnimationSource.h"
#import "FrameSink.h"
#import "PlaybackController.h"
#import "PlayerError.h"
#import "RenderCoordinator.h"

@class RNRlottiePlayer;

// Private hooks the SinkBridge calls back into (always on the main thread).
@interface RNRlottiePlayer ()
- (void)handleSinkEvent:(rnrlottie::PlayerEvent)event;
- (void)handleFramePublished:(std::uint64_t)generation;
- (NSDictionary *)dictionaryForMetadata:(const rnrlottie::AnimationMetadata &)md;
@end

namespace {

// FrameSink is a C++ abstract class, so an Obj-C object cannot implement it
// directly. This bridge does, holding a __weak back-pointer to the owning
// player. Its methods run ON THE RENDER WORKER; they capture a __weak local
// (never `this`) and marshal to the main thread, so a torn-down/dealloced
// player simply no-ops and there is no retain cycle or dangling `this`.
struct SinkBridge : rnrlottie::FrameSink {
    __weak RNRlottiePlayer *owner;

    explicit SinkBridge(RNRlottiePlayer *o) : owner(o) {}

    void onFramePublished(std::uint64_t generation) override {
        __weak RNRlottiePlayer *weakOwner = owner;
        dispatch_async(dispatch_get_main_queue(), ^{
            RNRlottiePlayer *strong = weakOwner;
            if (strong) {
                [strong handleFramePublished:generation];
            }
        });
    }

    void onEvent(const rnrlottie::PlayerEvent &event) override {
        __weak RNRlottiePlayer *weakOwner = owner;
        rnrlottie::PlayerEvent copy = event;  // copy off the worker's reference
        dispatch_async(dispatch_get_main_queue(), ^{
            RNRlottiePlayer *strong = weakOwner;
            if (strong) {
                [strong handleSinkEvent:copy];
            }
        });
    }
};

NSString *CodeString(rnrlottie::PlayerErrorCode code) {
    using C = rnrlottie::PlayerErrorCode;
    switch (code) {
        case C::InvalidSource: return @"INVALID_SOURCE";
        case C::SourceNotFound: return @"SOURCE_NOT_FOUND";
        case C::SourceTooLarge: return @"SOURCE_TOO_LARGE";
        case C::ParseFailed: return @"PARSE_FAILED";
        case C::InvalidDimensions: return @"INVALID_DIMENSIONS";
        case C::AllocationFailed: return @"ALLOCATION_FAILED";
        case C::RenderFailed: return @"RENDER_FAILED";
        case C::Released: return @"RELEASED";
        case C::None: return @"NONE";
    }
    return @"RENDER_FAILED";
}

NSString *ToNSString(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data()
                                    length:s.size()
                                  encoding:NSUTF8StringEncoding]
               ?: @"";
}

}  // namespace

@implementation RNRlottiePlayer {
    // _sink is declared before _coordinator so it is destroyed AFTER the
    // coordinator (reverse-declaration order): the coordinator must never
    // outlive the FrameSink it references. teardown enforces the same order.
    std::unique_ptr<SinkBridge> _sink;
    std::unique_ptr<rnrlottie::RenderCoordinator> _coordinator;
    rnrlottie::PlaybackController _playback;  // [main]-only
    BOOL _torndown;
}

- (instancetype)initWithMaxPixels:(size_t)maxPixels {
    if ((self = [super init])) {
        _torndown = NO;
        _sink = std::make_unique<SinkBridge>(self);
        _coordinator = std::make_unique<rnrlottie::RenderCoordinator>(
            *_sink, rnrlottie::FrameBuffer::Limits{maxPixels});
    }
    return self;
}

- (void)dealloc {
    [self teardown];
}

#pragma mark - Sink callbacks (main thread)

- (void)handleSinkEvent:(rnrlottie::PlayerEvent)event {
    if (_torndown) {
        return;
    }
    if (event.type == rnrlottie::PlayerEvent::Type::Loaded) {
        // Drive the state machine in lock-step with the load, then notify JS.
        _playback.onLoaded(event.metadata);
        if (self.onLoaded) {
            self.onLoaded([self dictionaryForMetadata:event.metadata]);
        }
    } else {
        _playback.onLoadFailed(event.error);
        if (self.onError) {
            self.onError(CodeString(event.error.code), ToNSString(event.error.message));
        }
    }
}

- (void)handleFramePublished:(std::uint64_t)generation {
    (void)generation;
    if (_torndown) {
        return;
    }
    // The view reads -frontBuffer (a consistent snapshot) and presents.
    if (self.onFramePresentable) {
        self.onFramePresentable();
    }
}

- (NSDictionary *)dictionaryForMetadata:(const rnrlottie::AnimationMetadata &)md {
    NSMutableArray *markers = [NSMutableArray arrayWithCapacity:md.markers.size()];
    for (const auto &m : md.markers) {
        [markers addObject:@{
            @"name": ToNSString(m.name),
            @"startFrame": @(m.startFrame),
            @"endFrame": @(m.endFrame),
        }];
    }
    return @{
        @"width": @(md.width),
        @"height": @(md.height),
        @"duration": @(md.duration),
        @"frameRate": @(md.frameRate),
        @"totalFrames": @(md.totalFrames),
        @"markers": markers,
    };
}

#pragma mark - Source

- (void)setSourceData:(NSString *)json
             cacheKey:(NSString *)cacheKey
         resourcePath:(NSString *)resourcePath
        useModelCache:(BOOL)useModelCache {
    if (_torndown || !_coordinator) {
        return;
    }
    rnrlottie::AnimationSource src;
    src.kind = rnrlottie::AnimationSource::Kind::Data;
    // Preserve exact bytes (JSON may contain multibyte sequences).
    NSData *data = [json dataUsingEncoding:NSUTF8StringEncoding] ?: [NSData data];
    src.json.assign(static_cast<const char *>(data.bytes), data.length);
    src.cacheKey = cacheKey ? std::string(cacheKey.UTF8String ?: "") : std::string();
    src.resourcePath =
        resourcePath ? std::string(resourcePath.UTF8String ?: "") : std::string();
    src.useModelCache = useModelCache;
    _coordinator->setSource(std::move(src));
}

- (void)setSourceFile:(NSString *)path useModelCache:(BOOL)useModelCache {
    if (_torndown || !_coordinator) {
        return;
    }
    rnrlottie::AnimationSource src;
    src.kind = rnrlottie::AnimationSource::Kind::File;
    src.path = path ? std::string(path.UTF8String ?: "") : std::string();
    src.useModelCache = useModelCache;
    _coordinator->setSource(std::move(src));
}

#pragma mark - Configuration & commands

- (void)configureLoop:(BOOL)loop
          repeatCount:(int)repeatCount
                speed:(double)speed
           startFrame:(NSInteger)startFrame
             endFrame:(NSInteger)endFrame
             autoPlay:(BOOL)autoPlay {
    rnrlottie::PlaybackConfig cfg;
    cfg.loop = loop;
    cfg.repeatCount = repeatCount;
    cfg.speed = speed;
    cfg.startFrame = startFrame > 0 ? static_cast<std::size_t>(startFrame) : 0;
    cfg.endFrame = endFrame > 0 ? static_cast<std::size_t>(endFrame) : 0;  // 0 => last
    cfg.autoPlay = autoPlay;
    _playback.configure(cfg);
}

- (void)playFromFrame:(NSInteger)startFrame toFrame:(NSInteger)endFrame {
    // Contract: -1 means "unset" (docs/bridge-contract.md's `play` command).
    // std::size_t can't represent negative values, so the sentinel check must
    // happen here rather than via a `>0` truthiness check (0 is a valid frame).
    const std::optional<std::size_t> sf =
        startFrame >= 0 ? std::optional<std::size_t>(static_cast<std::size_t>(startFrame))
                        : std::nullopt;
    const std::optional<std::size_t> ef =
        endFrame >= 0 ? std::optional<std::size_t>(static_cast<std::size_t>(endFrame))
                      : std::nullopt;
    _playback.play(sf, ef);
}

- (void)pause {
    _playback.pause();
}

- (void)resume {
    _playback.resume();
}

- (void)stop {
    _playback.stop();
}

- (void)reset {
    _playback.reset();
    if (_coordinator) {
        _coordinator->reset();  // bump generation, drop any in-flight render
    }
}

- (void)seekToFrame:(NSInteger)frame {
    _playback.seekToFrame(frame > 0 ? static_cast<std::size_t>(frame) : 0);
}

- (void)seekToProgress:(double)progress {
    _playback.seekToProgress(progress);
}

- (void)setSpeed:(double)speed {
    _playback.setSpeed(speed);
}

- (BOOL)playMarker:(NSString *)name {
    if (_torndown || name.length == 0) {
        return NO;
    }
    // [UI]-only, matching every other transport command above.
    return _playback.playMarker(std::string(name.UTF8String ?: "")) ? YES : NO;
}

- (void)setColorForKeyPath:(NSString *)keyPath
                        red:(double)red
                      green:(double)green
                       blue:(double)blue {
    if (_torndown || !_coordinator || keyPath.length == 0) {
        return;
    }
    _coordinator->setColor(std::string(keyPath.UTF8String ?: ""),
                            static_cast<float>(red),
                            static_cast<float>(green),
                            static_cast<float>(blue));
}

- (void)setOpacityForKeyPath:(NSString *)keyPath opacity:(double)opacity {
    if (_torndown || !_coordinator || keyPath.length == 0) {
        return;
    }
    _coordinator->setOpacity(std::string(keyPath.UTF8String ?: ""),
                              static_cast<float>(opacity));
}

- (void)setStrokeWidthForKeyPath:(NSString *)keyPath width:(double)width {
    if (_torndown || !_coordinator || keyPath.length == 0) {
        return;
    }
    _coordinator->setStrokeWidth(std::string(keyPath.UTF8String ?: ""),
                                  static_cast<float>(width));
}

- (void)setSurfaceWidth:(size_t)width height:(size_t)height {
    if (_torndown || !_coordinator) {
        return;
    }
    _coordinator->setSurfaceSize(width, height);
}

#pragma mark - Tick & presentation

- (void)onDisplayTick:(double)monotonicSeconds {
    if (_torndown || !_coordinator) {
        return;
    }
    const rnrlottie::PlaybackController::Tick tick = _playback.advance(monotonicSeconds);
    switch (tick.event) {
        case rnrlottie::PlaybackEvent::Started:
            if (self.onStart) self.onStart();
            break;
        case rnrlottie::PlaybackEvent::Paused:
            if (self.onPause) self.onPause();
            break;
        case rnrlottie::PlaybackEvent::Loop:
            if (self.onLoop) self.onLoop();
            break;
        case rnrlottie::PlaybackEvent::Finished:
            if (self.onFinish) self.onFinish();
            break;
        case rnrlottie::PlaybackEvent::None:
            break;
    }
    if (tick.shouldRender) {
        _coordinator->requestFrame(tick.frame, _coordinator->generation());
    }
}

- (rnrlottie::FrameBuffer::FrontBuffer)frontBuffer {
    if (_torndown || !_coordinator) {
        return rnrlottie::FrameBuffer::FrontBuffer{};
    }
    return _coordinator->frameBuffer().readFront();
}

#pragma mark - Teardown

- (void)teardown {
    if (_torndown) {
        return;
    }
    _torndown = YES;

    // release() bumps the generation, cancels pending work, destroys the core on
    // the worker, then JOINS — so no SinkBridge callback can fire afterwards.
    // Destroy the coordinator before the sink it references.
    if (_coordinator) {
        _coordinator->release();
        _coordinator.reset();
    }
    _sink.reset();

    // Any main-queue block already in flight will find these nil / _torndown set.
    self.onLoaded = nil;
    self.onError = nil;
    self.onStart = nil;
    self.onPause = nil;
    self.onLoop = nil;
    self.onFinish = nil;
    self.onFramePresentable = nil;
}

@end
