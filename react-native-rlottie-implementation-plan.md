# React Native rlottie library — implementation plan

## 1. Compatibility decision

A true React Native **Legacy Architecture / Bridge** implementation should target **React Native 0.81 or earlier**. React Native 0.82 became New-Architecture-only, and newer releases are progressively removing Legacy Architecture implementation classes. Legacy components may still work through interoperability layers in some versions, but that is not the same as running on the old bridge.

Recommended support contract:

| Target | Support |
|---|---|
| React Native ≤ 0.81 with `newArchEnabled=false` | Primary target |
| React Native 0.82+ | Not guaranteed in v1; test separately through interop |
| Fabric / TurboModules | Explicitly out of scope for v1 |
| iOS and Android | Required |
| Windows/macOS | Out of scope |

Do not publish the library as “all current React Native versions supported” unless an additional Fabric adapter is implemented.

---

## 2. Architectural approach

The package should be a **native UI component**, not a conventional Native Module that sends frames through the bridge.

```text
React / TypeScript
        │
        │ props, commands, events
        ▼
Legacy React Native Bridge
        │
        ├── iOS: RCTViewManager
        └── Android: SimpleViewManager
                    │
                    ▼
         Platform Player Adapter
                    │
                    ▼
       Shared C++ RlottiePlayerCore
                    │
                    ▼
        Vendored rlottie static library
```

React Native’s legacy native component APIs use an `RCTViewManager` on iOS and a registered `ViewManager` on Android.

### Main architectural rule

**There must be no per-frame JS-to-native bridge communication.**

JavaScript communicates only:

- source changes;
- playback configuration;
- imperative commands;
- infrequent lifecycle events.

The native layer owns:

- parsing;
- animation timing;
- frame selection;
- rendering;
- frame buffers;
- caching;
- dropping late frames;
- lifecycle cleanup.

---

## 3. Proposed public API

```tsx
import React, {useRef} from 'react';
import {
  RlottieView,
  type RlottieViewRef,
} from 'react-native-rlottie';

export function Example() {
  const animationRef = useRef<RlottieViewRef>(null);

  return (
    <RlottieView
      ref={animationRef}
      source={require('./animation.json')}
      autoPlay
      loop
      speed={1}
      resizeMode="contain"
      onAnimationLoaded={event => {
        console.log(event.nativeEvent.duration);
      }}
      onAnimationFinish={() => {
        console.log('Finished');
      }}
      onAnimationError={event => {
        console.error(event.nativeEvent.message);
      }}
      style={{width: 200, height: 200}}
    />
  );
}
```

### Source types

```ts
export type RlottieSource =
  | number
  | {
      uri: string;
      cacheKey?: string;
    }
  | {
      json: string | Record<string, unknown>;
      cacheKey: string;
    };
```

Recommended v1 support:

1. Imported JSON object.
2. Raw JSON string.
3. `file://` URI.
4. Android bundled asset.
5. iOS bundle resource.
6. `content://` URI on Android.

Recommended v1.1 support:

- `https://` remote animation;
- `.lottie` container;
- external raster assets;
- disk cache policy.

Remote loading should not block the core renderer design.

### Props

```ts
export interface RlottieViewProps extends ViewProps {
  source: RlottieSource;

  autoPlay?: boolean;
  loop?: boolean;
  repeatCount?: number;
  speed?: number;

  progress?: number;
  startFrame?: number;
  endFrame?: number;

  resizeMode?: 'contain' | 'cover' | 'stretch' | 'center';
  renderScale?: number;

  pauseWhenInactive?: boolean;
  cacheStrategy?: 'none' | 'model';

  colorOverrides?: Array<{
    keyPath: string;
    color: string;
  }>;

  onAnimationLoaded?: (
    event: NativeSyntheticEvent<RlottieLoadedEvent>,
  ) => void;

  onAnimationStart?: () => void;
  onAnimationPause?: () => void;
  onAnimationLoop?: () => void;
  onAnimationFinish?: () => void;

  onAnimationError?: (
    event: NativeSyntheticEvent<RlottieErrorEvent>,
  ) => void;
}
```

### Ref commands

```ts
export interface RlottieViewRef {
  play(options?: {
    startFrame?: number;
    endFrame?: number;
  }): void;

  pause(): void;
  resume(): void;
  stop(): void;
  reset(): void;

  seekToProgress(progress: number): void;
  seekToFrame(frame: number): void;

  setSpeed(speed: number): void;
}
```

For the old architecture, the TypeScript wrapper should hide `findNodeHandle` and `UIManager.dispatchViewManagerCommand`. Consumers should never call `UIManager` directly.

---

## 4. Shared C++ core

Create a platform-neutral façade around rlottie rather than exposing rlottie directly to JNI and Objective-C++.

### Core classes

```text
cpp/
├── RlottiePlayerCore.h/.cpp
├── AnimationSource.h
├── AnimationMetadata.h
├── PlaybackState.h
├── PlaybackController.h/.cpp
├── RenderCoordinator.h/.cpp
├── FrameBuffer.h/.cpp
├── ModelCacheController.h/.cpp
└── third_party/
    └── rlottie/
```

### `RlottiePlayerCore`

Responsibilities:

- own `std::unique_ptr<rlottie::Animation>`;
- load from file or JSON;
- expose metadata;
- manage frame range;
- convert time or progress to frame number;
- apply dynamic properties;
- render into caller-provided memory;
- serialize all access to the animation instance.

Suggested API:

```cpp
class RlottiePlayerCore final {
public:
    struct LoadResult {
        bool success;
        AnimationMetadata metadata;
        PlayerError error;
    };

    LoadResult loadFromFile(
        const std::string& path,
        bool useModelCache);

    LoadResult loadFromData(
        std::string json,
        const std::string& cacheKey,
        const std::string& resourcePath,
        bool useModelCache);

    bool renderFrame(
        std::size_t frame,
        std::uint32_t* pixels,
        std::size_t width,
        std::size_t height,
        std::size_t bytesPerLine,
        bool preserveAspectRatio);

    void setColor(
        const std::string& keyPath,
        float red,
        float green,
        float blue);

    AnimationMetadata metadata() const;

    std::size_t frameForProgress(double progress);
};
```

rlottie supports loading from files or raw JSON, querying frame rate, total frames, dimensions and duration, and rendering into a caller-provided ARGB premultiplied surface. It also supports property overrides using key paths.

### Playback state machine

```text
Empty
  │ load
  ▼
Loading ──────────────► Failed
  │ success
  ▼
Ready
  │ play
  ▼
Playing ◄─────────────► Paused
  │ stop                 │ resume
  ▼                      │
Stopped ─────────────────┘

Any state ── source changed ──► Loading
Any state ── view detached ───► Released
```

Do not model playback as several unrelated booleans. Use one state enum plus explicit transition methods.

---

## 5. Threading and rendering model

### Thread ownership

#### JavaScript thread

Only:

- sets props;
- issues commands;
- receives events.

#### Main/UI thread

Only:

- receives native view properties;
- runs the display callback;
- presents completed frames;
- manages view lifecycle.

#### Render worker

Responsible for:

- parsing JSON;
- creating `rlottie::Animation`;
- rendering;
- applying property overrides;
- reallocating frame buffers;
- destroying the animation instance.

The same animation object must never be rendered and mutated concurrently.

### Frame scheduling

Use platform display clocks:

- iOS: `CADisplayLink`;
- Android: `Choreographer.FrameCallback`.

Each display tick calculates the desired animation frame from a monotonic clock:

```text
animationTime =
    playbackStartOffset +
    elapsedMonotonicTime × speed

frame =
    startFrame +
    floor(animationTime × frameRate)
```

Then apply:

- segment range;
- repeat count;
- loop rules;
- playback direction, when supported.

### Render queue policy

Use **one render request in flight per view**.

When a new tick occurs:

1. Calculate the newest required frame.
2. If no render is running, start it.
3. If a render is already running, store only the newest requested frame.
4. When rendering finishes, present it.
5. Render the latest pending frame.
6. Discard all obsolete intermediate frames.

This prevents an overloaded device from accumulating a large queue and playing several seconds behind real time.

### `renderSync` versus rlottie asynchronous rendering

rlottie offers both synchronous and asynchronous rendering and recommends asynchronous rendering for performance.

Recommended rollout:

- Start with `renderSync()` on the library’s dedicated render worker.
- Keep all rendering away from the UI thread.
- Benchmark it against rlottie’s `render()`/`std::future` implementation.
- Switch only when the internal asynchronous renderer demonstrates measurable benefit.

Using `renderSync()` on your own worker initially gives better control over:

- cancellation;
- object destruction;
- source replacement;
- thread affinity;
- profiling;
- stale-frame rejection.

---

## 6. Generation-based cancellation

Native rendering cannot always be interrupted safely. Use logical cancellation.

```cpp
std::atomic<std::uint64_t> generation_{0};
```

Increment the generation when:

- source changes;
- view dimensions change;
- the view is detached;
- the animation is reset;
- the native object is being destroyed.

Every render request captures the current generation:

```cpp
const auto requestGeneration = generation_.load();

renderFrame(...);

if (requestGeneration != generation_.load()) {
    // Result is stale. Do not present it.
    return;
}
```

This removes race conditions where an old source finishes loading after a newer source has already been assigned.

---

## 7. Frame buffer strategy

Use preallocated reusable buffers.

```text
Front buffer: currently displayed
Back buffer: currently being rendered
```

For a width `W` and height `H`:

```text
buffer size = W × H × 4 bytes
```

Two 1080 × 1080 buffers require approximately 8.9 MB, excluding platform presentation objects.

Rules:

- do not allocate one buffer per frame;
- allocate only after layout dimensions are known;
- reuse buffers until pixel dimensions change;
- clamp maximum render area;
- check multiplication overflow before allocating;
- reject dimensions that exceed configured limits;
- clear transparent areas before rendering;
- use premultiplied alpha consistently.

### Resize handling

React Native layout is measured in density-independent points, but rendering requires physical pixels.

```text
pixelWidth  = layoutWidth  × screenScale × renderScale
pixelHeight = layoutHeight × screenScale × renderScale
```

Clamp the result to the configured memory/rendering budget.

On rapid layout changes:

- debounce buffer recreation;
- invalidate previous render generation;
- keep displaying the last valid frame until the new buffer is ready.

---

## 8. Pixel format validation spike

rlottie documents its default surface as `ARGB32_Premultiplied`.

Before implementing full playback, create a test animation containing:

- opaque red;
- opaque green;
- opaque blue;
- 50% transparent red;
- gradients;
- transparent edges.

Render frame zero on both platforms and validate:

- channel order;
- premultiplied alpha;
- stride;
- row orientation;
- transparency;
- color space.

Do not assume that the 32-bit rlottie representation can be passed directly to every native bitmap API without channel conversion.

---

## 9. iOS implementation

### Files

```text
ios/
├── RNRlottieView.h
├── RNRlottieView.mm
├── RNRlottieViewManager.mm
├── RNRlottiePlayer.h
├── RNRlottiePlayer.mm
├── RNRlottieFramePresenter.h
├── RNRlottieFramePresenter.mm
└── RNRlottieModule.mm
```

Use Objective-C++ (`.mm`) wherever C++ types are referenced.

### `RNRlottieViewManager`

Responsibilities:

- subclass `RCTViewManager`;
- export the component;
- export React props;
- expose view commands;
- create `RNRlottieView`;
- retrieve views safely through the RN view registry.

Props should be exported with:

```objc
RCT_EXPORT_VIEW_PROPERTY(autoPlay, BOOL)
RCT_EXPORT_VIEW_PROPERTY(loop, BOOL)
RCT_EXPORT_VIEW_PROPERTY(speed, double)
RCT_EXPORT_VIEW_PROPERTY(resizeMode, NSString *)
RCT_EXPORT_VIEW_PROPERTY(onAnimationLoaded, RCTDirectEventBlock)
RCT_EXPORT_VIEW_PROPERTY(onAnimationError, RCTDirectEventBlock)
```

Use custom property conversion for the source object.

### `RNRlottieView`

Responsibilities:

- own the platform player adapter;
- react to layout changes;
- create and stop `CADisplayLink`;
- present completed frames;
- pause on application background;
- resume according to previous state;
- detach safely.

### Presentation

Recommended initial implementation:

1. Render into the back C++ buffer.
2. Create a `CGImage` with the matching bitmap configuration.
3. Assign it to `self.layer.contents` on the main thread.
4. Swap front and back buffers.

Validate:

```objc
kCGBitmapByteOrder32Little
kCGImageAlphaPremultipliedFirst
```

against rlottie output during the pixel-format spike.

Avoid reconstructing expensive objects unnecessarily. Reuse color spaces and other immutable Core Graphics resources.

### Lifecycle

Observe:

- `UIApplicationDidEnterBackgroundNotification`;
- `UIApplicationWillEnterForegroundNotification`;
- window attachment/detachment.

`dealloc` must:

- invalidate `CADisplayLink`;
- mark the generation stale;
- prevent future callbacks;
- wait for or safely defer native resource destruction;
- release buffers.

---

## 10. Android implementation

### Files

```text
android/src/main/java/com/example/rlottie/
├── RlottiePackage.kt
├── RlottieViewManager.kt
├── RlottieView.kt
├── RlottieModule.kt
├── RlottieSourceResolver.kt
└── RlottieEvents.kt

android/src/main/cpp/
├── CMakeLists.txt
├── RlottieJni.cpp
├── JniPlayerHandle.cpp
└── AndroidFrameBuffer.cpp
```

### `RlottieViewManager`

Responsibilities:

- extend `SimpleViewManager<RlottieView>`;
- expose props with `@ReactProp`;
- expose commands through `getCommandsMap`;
- implement both integer and string command paths when needed for RN-version compatibility;
- create the custom view;
- clean up in `onDropViewInstance`.

The manager is registered through `ReactPackage.createViewManagers`.

### `RlottieView`

Responsibilities:

- own a native `long` handle;
- schedule frames through `Choreographer`;
- draw the current bitmap in `onDraw`;
- react to attach/detach;
- react to layout and density changes;
- handle activity/application lifecycle.

### JNI API

Keep JNI narrow:

```cpp
JNIEXPORT jlong JNICALL
nativeCreatePlayer(JNIEnv*, jobject);

JNIEXPORT jobject JNICALL
nativeLoadFromData(
    JNIEnv*,
    jobject,
    jlong handle,
    jstring json,
    jstring cacheKey,
    jstring resourcePath);

JNIEXPORT jboolean JNICALL
nativeRenderFrame(
    JNIEnv*,
    jobject,
    jlong handle,
    jobject bitmap,
    jlong frame,
    jboolean keepAspectRatio);

JNIEXPORT void JNICALL
nativeSetColor(...);

JNIEXPORT void JNICALL
nativeDestroyPlayer(JNIEnv*, jobject, jlong handle);
```

Never expose raw native pointers directly without validating the handle.

### Bitmap integration

Two possible implementations:

#### Preferred after validation

- allocate `Bitmap.Config.ARGB_8888`;
- lock pixels using `AndroidBitmap_lockPixels`;
- render directly or render followed by a channel conversion;
- unlock;
- invalidate the view.

#### Safe fallback

- render into a native temporary/front-back buffer;
- convert into Android bitmap format;
- draw using `Canvas.drawBitmap`.

The first option is faster but must not be selected until channel order and stride are verified.

### Lifecycle cleanup

`onDropViewInstance` and `onDetachedFromWindow` must:

- stop `Choreographer` callbacks;
- increment generation;
- stop future events;
- release JNI handle once in-flight work is safe;
- remove lifecycle listeners.

---

## 11. Old bridge command and event design

### Commands

Map stable numeric IDs:

```ts
enum RlottieCommand {
  Play = 1,
  Pause = 2,
  Resume = 3,
  Stop = 4,
  Reset = 5,
  SeekToProgress = 6,
  SeekToFrame = 7,
  SetSpeed = 8,
}
```

Keep these IDs stable across releases.

The JS wrapper resolves the native component command:

```ts
UIManager.dispatchViewManagerCommand(
  findNodeHandle(nativeRef),
  commandId,
  args,
);
```

Do not issue commands until the native ref is mounted.

### Events

Use direct native events:

```ts
type RlottieLoadedEvent = {
  width: number;
  height: number;
  duration: number;
  frameRate: number;
  totalFrames: number;
  markers: Array<{
    name: string;
    startFrame: number;
    endFrame: number;
  }>;
};

type RlottieErrorEvent = {
  code:
    | 'INVALID_SOURCE'
    | 'SOURCE_NOT_FOUND'
    | 'SOURCE_TOO_LARGE'
    | 'PARSE_FAILED'
    | 'UNSUPPORTED_FEATURE'
    | 'ALLOCATION_FAILED'
    | 'RENDER_FAILED';

  message: string;
};
```

Avoid an `onFrame` event by default. Sending an event for every frame would recreate the performance problem the native component is intended to solve.

---

## 12. Source resolver

Separate source resolution from animation parsing.

```text
JS Source
   │
   ▼
Platform SourceResolver
   │
   ├── bundled resource
   ├── file URI
   ├── content URI
   ├── raw JSON
   └── remote URI
          │
          ▼
   app-private cached file
          │
          ▼
   RlottiePlayerCore
```

### Rules

- C++ core should not perform HTTP requests.
- Native loaders should resolve resources to an app-controlled path or JSON string.
- Do not pass arbitrary filesystem resource roots directly from JavaScript.
- Generate cache keys from source identity and package version.
- For object sources, stringify only once in the TypeScript wrapper.
- Parsing must happen on the render/load worker.
- Source changes must invalidate existing pending operations.

### External image resources

rlottie’s `loadFromData` accepts a `resourcePath` for external resources.

For security:

- create one private directory per animation package;
- reject `..` traversal;
- canonicalize paths;
- prevent access outside the package directory;
- allow only known image extensions;
- apply file-count and file-size limits.

---

## 13. rlottie dependency strategy

### Do not use an unpinned release tag

The CMake project still identifies itself as version `0.2`, but the repository has received important security fixes through July 2026.

There have been multiple memory-safety and input-validation vulnerabilities affecting older rlottie revisions, including out-of-bounds reads/writes, excessive allocation and recursion-related problems.

Recommended policy:

1. Vendor rlottie source into the npm package.
2. Pin a full commit SHA containing all relevant security fixes.
3. Record the SHA in:
   - `THIRD_PARTY_NOTICES.md`;
   - package metadata;
   - native version constants.
4. Maintain a reproducible update script.
5. Run native fuzz and malformed-file tests before updating.
6. Never fetch rlottie from the network during an application build.

Git submodules are unsuitable as the only distribution mechanism because npm consumers may not receive initialized submodules.

### License

rlottie’s public headers use a permissive MIT-style license requiring the copyright and permission notice to remain included.

Include:

```text
THIRD_PARTY_NOTICES.md
licenses/rlottie-LICENSE.txt
```

---

## 14. Native build configuration

rlottie is written in C++14 and exposes build options for threading, caching, modules and shared/static linkage.

### Recommended rlottie options

```cmake
set(BUILD_SHARED_LIBS OFF)
set(LOTTIE_THREAD ON)
set(LOTTIE_CACHE ON)
set(LOTTIE_MODULE OFF)
set(LOTTIE_TEST OFF)
```

Reasons:

- static linking simplifies distribution;
- thread support is required for intended performance;
- model caching is useful but should remain configurable;
- dynamic loader modules are undesirable in a mobile npm package;
- upstream examples and tests should not become application targets.

### Android CMake

```cmake
cmake_minimum_required(VERSION 3.18)

project(react_native_rlottie LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LOTTIE_THREAD ON CACHE BOOL "" FORCE)
set(LOTTIE_CACHE ON CACHE BOOL "" FORCE)
set(LOTTIE_MODULE OFF CACHE BOOL "" FORCE)
set(LOTTIE_TEST OFF CACHE BOOL "" FORCE)

add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../cpp/third_party/rlottie
    ${CMAKE_CURRENT_BINARY_DIR}/rlottie
)

add_library(
    react-native-rlottie
    SHARED
    RlottieJni.cpp
    JniPlayerHandle.cpp
    ../../../cpp/RlottiePlayerCore.cpp
    ../../../cpp/PlaybackController.cpp
    ../../../cpp/RenderCoordinator.cpp
)

target_link_libraries(
    react-native-rlottie
    PRIVATE
    rlottie::rlottie
    android
    log
    jnigraphics
)
```

Build at minimum:

- `arm64-v8a`;
- `armeabi-v7a`, when required by the host application;
- `x86_64` for emulator testing.

### iOS podspec

Compile the shared C++ core, rlottie sources and Objective-C++ adapter through CocoaPods.

```ruby
s.source_files = [
  'ios/**/*.{h,m,mm}',
  'cpp/**/*.{h,hpp,cpp,c}'
]

s.pod_target_xcconfig = {
  'CLANG_CXX_LANGUAGE_STANDARD' => 'c++14',
  'CLANG_CXX_LIBRARY' => 'libc++'
}
```

Exclude:

- upstream examples;
- tests;
- desktop tools;
- unsupported platform sources.

---

## 15. Model cache design

rlottie provides a library-level model cache size configuration.

Expose an optional old-architecture Native Module only for global operations:

```ts
Rlottie.configure({
  modelCacheSize: 16,
});

Rlottie.clearModelCache();
Rlottie.getNativeVersion();
```

Do not route per-view playback through this module.

### Cache key rules

A cache key must include:

```text
source identity
+ source content version/hash
+ rlottie commit/version
+ parsing configuration
```

Never let two different JSON payloads share the same caller-provided cache key silently.

A safer internal key:

```text
sha256(json bytes) + ":" + callerCacheKey
```

---

## 16. Security boundaries

Because Lottie input is parsed by native C++, source handling must be treated as untrusted.

Mandatory controls:

- pin a patched rlottie commit;
- reject sources above a configurable byte limit;
- reject zero, negative or overflowing dimensions;
- cap rendered pixel count;
- cap frame count and duration where appropriate;
- cap external asset count and total bytes;
- canonicalize every resource path;
- disallow filesystem escape;
- use app-private directories;
- allow-list URI schemes;
- use HTTPS only for remote content by default;
- do not automatically execute dynamic image-loader plugins;
- handle allocation failures without process termination;
- fuzz load and rendering paths;
- do not include raw animation JSON in production logs.

For remote animations, consider making remote loading opt-in:

```tsx
<RlottieView allowRemoteSources source={{uri: 'https://...'}} />
```

---

## 17. Error-handling policy

C++ should not throw exceptions across JNI or Objective-C++ boundaries.

Return typed results:

```cpp
enum class PlayerErrorCode {
    None,
    InvalidSource,
    SourceNotFound,
    SourceTooLarge,
    ParseFailed,
    InvalidDimensions,
    AllocationFailed,
    RenderFailed,
    Released,
};
```

Every error should have:

- stable machine-readable code;
- developer-readable message;
- optional native diagnostic field in development builds;
- source generation number.

Native errors should be converted into one consistent React event format.

---

## 18. Proposed repository structure

```text
react-native-rlottie/
├── src/
│   ├── RlottieView.tsx
│   ├── RlottieNativeComponent.ts
│   ├── RlottieModule.ts
│   ├── commands.ts
│   ├── source.ts
│   ├── types.ts
│   └── index.ts
│
├── cpp/
│   ├── RlottiePlayerCore.h
│   ├── RlottiePlayerCore.cpp
│   ├── PlaybackController.h
│   ├── PlaybackController.cpp
│   ├── RenderCoordinator.h
│   ├── RenderCoordinator.cpp
│   ├── FrameBuffer.h
│   ├── FrameBuffer.cpp
│   └── third_party/
│       └── rlottie/
│
├── ios/
│   ├── RNRlottieView.h
│   ├── RNRlottieView.mm
│   ├── RNRlottieViewManager.mm
│   ├── RNRlottiePlayer.mm
│   ├── RNRlottieFramePresenter.mm
│   └── RNRlottieModule.mm
│
├── android/
│   ├── build.gradle
│   └── src/main/
│       ├── cpp/
│       │   ├── CMakeLists.txt
│       │   ├── RlottieJni.cpp
│       │   └── JniPlayerHandle.cpp
│       └── java/.../
│           ├── RlottiePackage.kt
│           ├── RlottieViewManager.kt
│           ├── RlottieView.kt
│           ├── RlottieSourceResolver.kt
│           └── RlottieModule.kt
│
├── example/
├── tests/
│   ├── cpp/
│   ├── fixtures/
│   ├── golden/
│   └── malformed/
│
├── react-native-rlottie.podspec
├── THIRD_PARTY_NOTICES.md
└── package.json
```

---

## 19. Implementation phases

### Phase 0 — compatibility and rendering spike

Deliverables:

- lock the React Native support range;
- pin a patched rlottie commit;
- compile rlottie on iOS device/simulator;
- compile rlottie for Android ABIs;
- render one static frame;
- verify pixel format;
- verify alpha and channel order;
- measure native output dimensions and stride.

Exit criterion:

> The same fixture renders visually and pixel-semantically correctly on iOS and Android.

### Phase 1 — shared C++ engine

Implement:

- `RlottiePlayerCore`;
- source loading;
- metadata extraction;
- frame rendering;
- state machine;
- playback range calculation;
- reusable frame buffers;
- generation cancellation;
- typed errors;
- core unit tests.

Exit criterion:

> C++ tests can load, seek, render, loop and safely replace sources without platform UI code.

### Phase 2 — iOS native component

Implement:

- `RNRlottieView`;
- `RCTViewManager`;
- props;
- direct events;
- commands;
- `CADisplayLink`;
- render worker;
- Core Graphics presenter;
- foreground/background handling;
- cleanup and generation invalidation.

Exit criterion:

> Multiple animations can mount, play, resize and unmount repeatedly without blocking the main thread or leaking native resources.

### Phase 3 — Android native component

Implement:

- JNI wrapper;
- native handle safety;
- custom Android `View`;
- `SimpleViewManager`;
- props and commands;
- `Choreographer`;
- Bitmap presenter;
- lifecycle handling;
- Gradle/CMake integration.

Exit criterion:

> Android behaviour matches iOS for playback, events, source replacement and error handling.

### Phase 4 — TypeScript API

Implement:

- typed component;
- source normalization;
- imperative ref;
- hidden command dispatch;
- event types;
- prop validation;
- package exports;
- autolinking metadata.

Exit criterion:

> Consumers interact only with the documented TypeScript API and never access platform-specific methods.

### Phase 5 — caching and source hardening

Implement:

- model cache configuration;
- source byte limits;
- dimension and pixel-area limits;
- secure resource paths;
- deterministic cache keys;
- remote loading policy;
- malformed input fixtures.

Exit criterion:

> Invalid or hostile sources fail predictably without uncontrolled memory use or filesystem access.

### Phase 6 — dynamic properties

Implement after core stability:

- fill color overrides;
- stroke color overrides;
- opacity;
- stroke width;
- transform values where required;
- marker playback.

Property overrides are executed on the same serialized native queue as rendering.

### Phase 7 — performance and stability

Measure:

- JSON parsing duration;
- first-frame duration;
- frame render p50/p95/p99;
- dropped frames;
- buffer allocation count;
- peak native memory;
- UI-thread stalls;
- multiple simultaneous animations;
- background/foreground transitions.

Exit criterion:

> Performance budgets are documented against named reference devices and representative animation fixtures.

### Phase 8 — publication

Deliver:

- installation guide;
- supported RN matrix;
- API reference;
- troubleshooting guide;
- third-party license notices;
- example application;
- release notes;
- ABI verification;
- npm package content verification.

---

## 20. Testing strategy

### Shared C++ tests

Test:

- valid JSON;
- invalid JSON;
- empty animation;
- single-frame animation;
- very high frame count;
- extreme dimensions;
- invalid segment ranges;
- progress clamping;
- negative and zero speed policy;
- repeat count;
- source replacement during render;
- destruction during pending render;
- color override key paths;
- allocation overflow.

### Golden frame tests

For fixed fixtures:

1. Render selected frames.
2. Hash or compare pixel buffers.
3. Account for permitted platform conversion differences.
4. Store expected metadata and frame snapshots.

Fixtures should include:

- shapes;
- masks;
- gradients;
- transparency;
- text;
- images;
- precompositions;
- markers;
- malformed and recursive structures.

### Native lifecycle tests

- mount/unmount hundreds of times;
- navigate away while loading;
- change source rapidly;
- resize while rendering;
- background during playback;
- resume after foreground;
- multiple views using the same source;
- low-memory notifications;
- JS reload;
- React root destruction.

### Bridge tests

- every prop maps correctly;
- every command reaches the correct view;
- events are not sent after unmount;
- command issued before mount is handled safely;
- invalid command arguments do not crash;
- event payloads match TypeScript definitions.

---

## 21. Non-functional acceptance criteria

The first production release should satisfy these requirements:

- no JSON parsing on the main/UI thread;
- no frame pixels transferred through the React Native bridge;
- no per-frame React event;
- no unbounded render queue;
- no per-frame native memory allocation;
- no native callback after view destruction;
- identical command semantics on iOS and Android;
- deterministic handling of source replacement;
- explicit maximum input and render sizes;
- patched and pinned rlottie dependency;
- clean ASan/UBSan test execution for the C++ layer;
- clean native leak testing;
- documented React Native upper and lower support boundaries.

---

## 22. Main risks

| Risk | Mitigation |
|---|---|
| Old Architecture no longer exists in current RN releases | Target RN ≤ 0.81 explicitly; separate future Fabric adapter |
| rlottie native parsing vulnerabilities | Pin current patched commit, input limits, fuzzing |
| Pixel format mismatch | Mandatory cross-platform pixel-format spike |
| UI-thread jank | Dedicated parse/render queue and native display clocks |
| Render queue growth | Single in-flight render and latest-frame-wins policy |
| Use-after-free during unmount | Generation tokens, serialized ownership, safe deferred destruction |
| Excessive memory at large view sizes | Pixel-area cap and reusable double buffers |
| Android/iOS behavioural divergence | Shared playback controller and cross-platform contract tests |
| Large JSON crossing the bridge | Cross once during loading; support file sources and native resolution |
| External assets escaping resource directory | Canonicalized app-private paths and strict allow-listing |

The most important foundation is the separation between the **stable public TypeScript API**, the **legacy bridge adapters**, and the **shared C++ player core**. That boundary allows a future Fabric component to reuse the same renderer without redesigning playback, source handling, caching, or security.
