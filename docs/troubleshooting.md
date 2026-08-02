# Troubleshooting

Symptoms below are grouped by what you're actually seeing. Each entry states
the likely cause and how to confirm/fix it. See `docs/api-reference.md` for
the full prop/event/error reference and `docs/bridge-contract.md` for the
frozen prop/event/command contract this is all built against.

## Blank / black view, nothing renders

1. **Check for `onAnimationError` first.** Every load failure is reported
   there with a `code` from `RlottieErrorCode` (see below) — a blank view with
   no error event is a different problem than a blank view _with_ one.
2. **The view has zero layout size.** `RlottieView` is a native view like any
   other `View` — if its parent doesn't give it width/height (no `style`, a
   flex context that collapses it to 0, etc.), there's nothing to draw into.
   Native buffers are allocated only after layout dimensions are known
   (CLAUDE.md's "no per-frame allocation" invariant), so a 0×0 view never
   produces a frame.
3. **`autoPlay` is `false` and nothing called `play()`.** The default is not
   to auto-start; if you rely on an imperative `ref.play()` in an effect,
   confirm the effect actually runs and that the ref is populated (commands
   sent before the native view mounts are silently dropped, not queued — see
   "Imperative ref" in `docs/api-reference.md`).
4. **`progress`/`startFrame`/`endFrame` land the animation on a frame that
   happens to be visually empty** (e.g. `endFrame` pointing past the last
   drawn shape in a badly authored source). Try `seekToProgress(0.5)` to rule
   out a source-authoring issue.

## `onAnimationError` with `code: 'INVALID_SOURCE'`

Most common causes, in order of likelihood:

- **A `{path}` or `resourcePath` outside app-private storage.** Both native
  resolvers canonicalize the path and reject anything that resolves outside
  the app sandbox (`filesDir`/`cacheDir`/`noBackupFilesDir` on Android, the
  app container on iOS) — including via a `..` segment or a symlink that
  escapes it. This is a hard rejection, not a clamp. If you're passing a path
  from a third-party file picker or a `content://` copy, confirm the
  destination is actually inside your app's private directories before
  passing it as `{path}`.
- **A `{uri}` with an unsupported scheme.** Only `file:`, `asset:`, and (on
  Android) `content:` are accepted unconditionally; `http://` is always
  rejected and `https://` is rejected in v1 regardless of
  `allowRemoteSources` (remote fetch is v1.1 scope — see
  `docs/api-reference.md`). Check the exact scheme string in the error
  `message`.
- **`source.json` failed to `JSON.stringify`** (circular reference, a
  `toJSON()` that throws). This is caught client-side in `src/source.ts` and
  reported as `INVALID_SOURCE` before anything reaches native.
- **An empty `source.json` string, or a `source` missing all of `json`/`uri`/
  `path`.**

## `onAnimationError` with `code: 'SOURCE_TOO_LARGE'` or `'PARSE_FAILED'`

These come from `cpp/InputLimits.h`'s enforced limits (defaults; overridable
process-wide via `Rlottie.configure()`, though `configure()` currently only
exposes `modelCacheSize` from JS — the byte/pixel/frame/depth limits
themselves are set via the native `InputLimits` API, not yet from JS):

| limit          | default            | rejection                                            |
| -------------- | ------------------ | ---------------------------------------------------- |
| `maxJsonBytes` | 16 MiB             | `SOURCE_TOO_LARGE`                                   |
| `maxFrames`    | 100,000            | `SOURCE_TOO_LARGE`                                   |
| `maxJsonDepth` | 512 nesting levels | `PARSE_FAILED`                                       |
| `maxPixels`    | 4096×4096          | `INVALID_DIMENSIONS` (not this section, but related) |

**Why JSON nesting depth is checked at all, and why it's a crash guard rather
than a policy knob**: rlottie's vendored rapidjson parses with recursive
descent (no iterative parsing flag), so a deeply nested JSON document
recurses once per level and can exhaust the native call stack — an
unrecoverable `SIGSEGV`, not something a try/catch or a typed error code can
turn into a graceful failure. Critically, **the byte-size limit does not bound
nesting depth**: a nesting token in a real Lottie shape tree is only ~18
bytes, and a pathological `[[[[…` is one byte per level, so the 16 MiB
default alone would still permit roughly 900k+ levels of legitimate-looking
nesting. `checkJsonDepth()` scans the JSON once (string-aware, so brackets
inside string literals don't count) and rejects over-deep input before
rlottie's parser ever sees it. If you hit `PARSE_FAILED` on a source that
looks otherwise valid, check whether it's unusually deeply nested — 512
levels is far beyond what a normally authored Lottie file needs.

**This guard only covers the in-memory `{json}` path.** A `{path}`/`{uri}`
source is handed to rlottie's own file loader, which reads and parses the
bytes itself — the depth check never runs on that path. The source resolvers
confine file sources to app-private storage, which narrows the attack surface
but doesn't eliminate it (a `content://` copy or a downloaded asset can still
land in app-private storage). If you accept files from an untrusted origin,
prefer routing them through `{json}` if at all practical, or treat file-backed
sources as inherently less guarded against pathological nesting.

## `onAnimationError` with `code: 'INVALID_DIMENSIONS'`

The parsed composition (or the render surface) has zero width/height, a
dimension outside 32-bit range, or a pixel count over `maxPixels` (4096×4096
default). A Lottie authored at an unusually large canvas size, or a
`renderScale` pushing an already-large view over the pixel cap, will trip
this.

## Native module or component "not found" / linking errors

- **`The native module "RlottieModule" is not available"` at runtime (calling
  `Rlottie.configure()`/`clearModelCache()`/`getNativeVersion()`).** This is
  the deliberate lazy-resolution error from `src/RlottieModule.ts` — the
  native module is only looked up when one of these functions is actually
  called, not at import time (so importing `RlottieView` never crashes a
  native-less environment like Jest or Storybook). Fix: confirm `pod install`
  ran (iOS) and that you did a full rebuild after installing — a Metro/Fast
  Refresh reload does not pick up new native code.
- **iOS: `RlottieView` component not found, or `pod install` doesn't see the
  package.** Confirm `react-native.config.js` at the package root declares
  `dependency.platforms.ios: {}` (it does, by default) and that autolinking
  isn't disabled for this package in the consuming app's own
  `react-native.config.js` (`dependencies.react-native-rlottie.platforms.ios:
null`, or similar exclusion).
- **Android: `ClassNotFoundException` for `RlottieView`/`RlottieBridge` at
  runtime, but the native `.so` loads fine.** This means the Kotlin classes
  were never compiled into the AAR. `android/build.gradle` applies
  `org.jetbrains.kotlin.android` specifically because without it, every `.kt`
  file under `src/main/java` is silently excluded from the build — the AAR
  ships the native library with no Java/Kotlin classes at all, and this fails
  at runtime with `ClassNotFoundException`, not at build time. If you're
  vendoring/forking this library's Android build script, verify the Kotlin
  plugin is still applied.
- **`newArchEnabled=true` and the component silently doesn't render, or
  autolinking behaves unexpectedly.** This library is **Legacy Architecture
  only** — there's no Fabric component or Codegen spec. Set
  `newArchEnabled=false` (iOS: `RCT_NEW_ARCH_ENABLED=0`; Android:
  `newArchEnabled=false` in `gradle.properties`) for apps using this library.

## Android: library loads, but crashes/fails on the very first native call

This is a **link-time symbol-visibility failure**, distinct from a normal
crash — the `.so` loads successfully (`System.loadLibrary` succeeds) and then
the very first JNI call (e.g. the first `nativeCreate`) fails as if the symbol
doesn't exist. If you're building this library from source (not consuming a
prebuilt AAR) and hit this, the cause is almost certainly one of:

- **rlottie's own linker version script (`rlottie.expmap`) leaking onto our
  `.so`.** rlottie ends its own version script with `local: *;`, which hides
  every symbol not explicitly exported. Because rlottie is linked as a static
  library here, CMake propagates a static library's `PRIVATE` link items
  (including its version script) to consumers — so without an explicit fix,
  that script lands on our own `.so` and hides **every** `Java_com_rlottie_*`
  JNI entry point. `android/src/main/cpp/CMakeLists.txt` strips rlottie's
  interface version script and applies our own
  `react-native-rlottie.expmap` instead. If you've modified the CMake linking
  here, confirm that override is still in place.
- Verify with `scripts/check-android-build.sh --link [<api-level>]`, which
  builds the real `.so` per ABI and asserts the `Java_com_rlottie_*` symbols
  are actually exported — a syntax-only build (the default,
  `scripts/check-android-build.sh` without `--link`) cannot catch this class
  of failure, because it type-checks each JNI translation unit without ever
  linking.

## Android: `armeabi-v7a` fails to build/link

The NDK defines `__ARM_NEON__` on `armeabi-v7a`, which causes rlottie to
compile its NEON-accelerated path (`vdrawhelper_neon.cpp`), which in turn
needs pixman's NEON assembly (`.S` files, GNU-as syntax). NDK r28+ ships no
GNU assembler, and the NDK's integrated Clang assembler rejects that syntax
outright, so the ABI fails to link. This library undefines `__ARM_NEON__` for
rlottie specifically on `armeabi-v7a` (mirroring the same fix the podspec
already applies for Apple arm64, where the same NEON-detection default
applies) and falls back to the generic (non-NEON) render path on that ABI
only. `armeabi-v7a` is not built by default (`arm64-v8a` + `x86_64` are); it's
opt-in via `rlottieAbiFilters` in the consuming app's `android/build.gradle`.
If you enable it and hit a link failure referencing pixman NEON symbols,
confirm this undef is still applied for that ABI specifically.

## Android: duplicate `libc++_shared.so` / app merge failure

React Native itself already ships `libc++_shared.so`; if this library's AAR
also includes a copy, the app's native-library merge step fails on the
duplicate. `android/build.gradle`'s `packagingOptions.excludes` drops
`**/libc++_shared.so` from this library's own AAR for exactly this reason. If
you see this failure, check whether some other dependency or a local build
override reintroduced the duplicate — it should not come from this library
under normal autolinking.

## An override (`colorOverrides`/`opacityOverrides`/`strokeWidthOverrides`) doesn't seem to apply

- **Applied after the animation is already loaded, to a layer with no
  animated keyframes.** This vendored rlottie skips re-evaluating a layer's
  paint on repeat renders when nothing about that layer is animated, so a
  `setValue()`-style override (which is what all three override props route
  to) added after load may have no visible effect on such a layer. This is
  pre-existing rlottie engine behavior, not a bug specific to this library —
  set the override before the source loads (or accept that static layers may
  not pick up a later override) if this matters for your use case.
- **Stroke-only shape + `opacityOverrides`.** rlottie's opacity control is
  `FillOpacity`; there is no combined fill+stroke opacity property, so a
  shape with no fill (stroke only) is unaffected by an opacity override. This
  mirrors `colorOverrides`, which has the same fill-only scope.
- **`colorOverrides` alpha looks ignored.** It is — both platforms discard the
  alpha channel of `#AARRGGBB`; `RenderCoordinator::setColor` has no alpha
  parameter. Only the RGB channels take effect.
- Remember all three overrides apply on the render worker, not synchronously
  on the calling thread — expect the effect on a subsequent displayed frame,
  not instantaneously.

## `play({marker})` seems to ignore `startFrame`/`endFrame`

This is intentional, not a bug: when `marker` is set in `RlottiePlayOptions`,
it **wins outright** over `startFrame`/`endFrame` in the same call — they are
not merged, because a named segment and an explicit frame range have no
coherent combination. If you need a specific frame range, omit `marker`.

An unknown marker name is a **silent no-op** (native returns a bool the
adapters ignore) — it does _not_ emit `onAnimationError`, since a missing
marker is treated as a caller mistake rather than an animation failure. If
`play({marker: 'x'})` appears to do nothing, first confirm `'x'` actually
appears in the `markers` array of the `onAnimationLoaded` event for that
source.

## Reading `onMetrics`

`onMetrics` only fires when `metricsEnabled` is `true`, at most once per
second — it is not a per-frame event and should not be treated as one.

- **`framesDropped` increasing under load is expected, correct behavior**,
  not a symptom to chase down. The library keeps at most one render in
  flight per view (latest-frame-wins); under load it discards stale render
  requests rather than queuing them, and `framesDropped` counts exactly that
  coalescing. A rising count means the anti-backlog policy is doing its job.
- **`peakBufferBytes` is this view's own frame buffers, not process RSS.**
  There's no portable way to read total process memory from inside a
  library, so don't use this field as a proxy for the app's overall memory
  footprint — it only reflects buffers this one `RlottieView` instance holds.
- **`bufferAllocCount` should stop increasing once the view reaches a stable
  size.** If it keeps climbing during steady playback (no resize, no source
  change), that indicates a real problem — buffers are meant to be allocated
  once per size and reused every frame after.
- **Platform payload divergence**: Android's four integer fields
  (`framesRendered`, `framesDropped`, `bufferAllocCount`, `peakBufferBytes`)
  are written via `WritableMap.putInt`, which **saturates** at 32 bits
  (`2147483647`) rather than overflowing; iOS boxes the native `uint64_t`
  exactly. This only matters if you've raised `InputLimits.maxPixels` far
  enough to push `peakBufferBytes` past ~2 billion — the default limits keep
  every field well under that.
- `uiStallCount`/`uiStallMaxMs` are measured on the platform display clock
  (`CADisplayLink`/`Choreographer`) and reflect ticks that arrived later than
  expected — a spike here points at UI-thread contention elsewhere in your
  app, not necessarily at this library.

## Performance concerns

If you're evaluating whether this library's render path is fast enough,
`docs/render-benchmark.md` has the only numbers in this repo, and they come
with a load-bearing caveat: **they were measured on a development Mac, not a
phone**, and are explicitly not device-validated. Don't extrapolate them to
production mobile hardware.
