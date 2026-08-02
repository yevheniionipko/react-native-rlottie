# Changelog

All notable changes to this project are documented in this file. The format
is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-08-02

Initial v1 feature set — the Legacy-Architecture-only React Native component
for rendering Lottie animations via the vendored rlottie C++ engine.

### Added

- `RlottieView` — a native UI component (iOS `RCTViewManager` /
  Objective-C++, Android `SimpleViewManager` / JNI) over a shared C++ core
  (`cpp/RlottiePlayerCore` and friends). No per-frame bridge traffic: no
  `onFrame` event, no frame pixels cross the JS↔native bridge.
- 13 declarative props: `source`, `allowRemoteSources`, `autoPlay`, `loop`,
  `repeatCount`, `speed`, `progress`, `startFrame`, `endFrame`, `resizeMode`,
  `renderScale`, `pauseWhenInactive`, `cacheStrategy`, `metricsEnabled`, plus
  the three dynamic-property override arrays `colorOverrides`,
  `opacityOverrides`, `strokeWidthOverrides`.
- An imperative ref (`RlottieViewRef`): `play`, `pause`, `resume`, `stop`,
  `reset`, `seekToProgress`, `seekToFrame`, `setSpeed` — dispatched through a
  stable command-resolution path that avoids the iOS legacy-bridge's raw
  integer-index fragility (`src/commands.ts`).
- `play({marker})` — plays a named marker's frame segment from the source
  (command id 9, added after the initial command set was frozen; the id
  space is append-only going forward).
- Six lifecycle/error events (`onAnimationLoaded`, `onAnimationError`,
  `onAnimationStart`, `onAnimationPause`, `onAnimationLoop`,
  `onAnimationFinish`) plus the opt-in, throttled (≤1/sec) `onMetrics` event
  for render/frame/buffer instrumentation.
- Source resolution for `{json}` (string or parsed object), `{uri}`
  (`file://`, `asset:///` — identical scheme on both platforms, `content://`
  on Android), and `{path}` (pre-resolved absolute path, confined to
  app-private storage). Content-addressed cache keys
  (`sha256(content):callerKey`, extended natively with the rlottie commit and
  parse config) so a caller-supplied cache key alone can never alias two
  different payloads.
- `Rlottie` global module: `configure({modelCacheSize})`,
  `clearModelCache()`, `getNativeVersion()`, `isAvailable()` — all
  Promise-based on both platforms.
- Native input hardening (`cpp/InputLimits.h`): byte-size limit (16 MiB
  default), pixel-area limit (4096×4096 default), frame-count limit (100,000
  default), and a JSON-nesting-depth guard (512 levels default) that exists
  specifically to stop a deeply nested (but small) payload from crashing the
  process via recursive-descent JSON parsing.
- Full instrumentation surface (`cpp/Metrics.h`): parse/first-frame timing,
  render-duration percentiles over a bounded rolling window, frame
  rendered/dropped counts, buffer allocation count and peak size, and
  display-clock stall tracking.
- Android build wiring that resolves the correct React Native Maven artifact
  across the 0.71 `react-native` → `react-android` rename, applies the
  Kotlin Gradle plugin (required for the Kotlin sources to be compiled into
  the AAR at all), and excludes the duplicate `libc++_shared.so`.
- `armeabi-v7a` support (opt-in), working around the NDK's GNU-as-only
  pixman NEON assembly by disabling `__ARM_NEON__` for rlottie on that ABI
  specifically (the same workaround the podspec already applies on Apple
  arm64).
- A pinned, vendored rlottie build (`cpp/third_party/rlottie/`, commit
  recorded in `cpp/RlottieVersion.h`) — never fetched at build time.

### Fixed

- `PlaybackController::play(start, end)` was persisting its one-off frame
  range into the persistent playback config, so a later argument-less
  `play()` incorrectly inherited a previous call's range. `play()` now
  re-derives from the persistent config on every call before applying an
  override.
- `FrameBuffer::resize()` only caught `std::bad_alloc`; a resize request
  large enough to exceed `std::vector::max_size()` throws
  `std::length_error` instead, which was uncaught and terminated the
  process. Now mapped to `ALLOCATION_FAILED` like any other allocation
  failure.

### Known limitations

- **Legacy Architecture only.** No Fabric component, no TurboModule spec, no
  Codegen in this release. The layering is designed so a future Fabric
  adapter can reuse the shared C++ core, but that adapter does not exist yet.
- **`resizeMode` and `cacheStrategy` are iOS-only in effect.** Both are
  accepted and validated on Android but are currently no-ops there: Android's
  draw path always stretches to view bounds, and its source natives take no
  cache-strategy flag yet.
- **`colorOverrides` discards alpha** on both platforms — only RGB is
  applied.
- **`opacityOverrides` has no effect on stroke-only shapes** — it maps to
  rlottie's `FillOpacity`, and rlottie has no combined fill+stroke opacity
  control.
- **An override added after load may not affect a layer with no animated
  keyframes** — pre-existing rlottie behavior (this vendored engine skips
  re-evaluating a static layer's paint on repeat renders).
- **The JSON-nesting-depth crash guard only covers the in-memory `{json}`
  path.** `{path}`/`{uri}` sources are parsed by rlottie's own file reader and
  are not covered by this JS-observable check, though the resolvers still
  confine file sources to app-private storage.
- **`allowRemoteSources` is a stub.** It only changes the wording of the
  rejection message for an `https://` source; the native layer performs no
  network I/O in this release (remote fetch is v1.1 scope).
- **`onMetrics`'s four integer fields saturate at 32 bits on Android**
  (`WritableMap.putInt`) while iOS boxes the native `uint64_t` exactly —
  unreachable at default limits, reachable if `InputLimits.maxPixels` is
  raised far enough via native configuration.
- **File-backed cache keys are location-derived, not content-derived**: two
  different files written to the same path produce the same cache key unless
  the caller also changes `cacheKey` or the path.
- **`maxExternalAssets`/`maxExternalBytes` are enforced by the resolvers, not
  the core** — rlottie exposes no pre-parse API to enumerate an animation's
  embedded external assets.
- **Only RN 0.81.0 is built and exercised in this repository.** `0.68.0` –
  `0.80.x` are expected to work but are not independently verified here — see
  the compatibility matrix in `README.md`.
- **Render-path performance numbers (`docs/render-benchmark.md`) are
  development-Mac measurements, not device-validated.** On-device golden
  verification is planned but not yet part of this release.
- **This release has not been run in a real app on a physical device or
  emulator.** The example app scaffold exists but building and exercising it
  end-to-end is follow-up work.
