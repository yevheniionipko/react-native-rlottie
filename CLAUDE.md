# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

**Phase 0 and Phase 1 are complete** (see `detailed-implementation-plan.md`).
Built and verified so far:

- Repo scaffold + tooling (`package.json`, `tsconfig`, lint/format configs).
- Vendored + pinned rlottie (`cpp/third_party/rlottie/`, `scripts/update-rlottie.sh`).
- Native build wiring: `android/` (CMake/Gradle) + `react-native-rlottie.podspec`.
- Pixel-format report + golden (`docs/pixel-format-report.md`, `cpp/PixelFormat.h`).
- The full shared C++ core in `cpp/`: `RlottiePlayerCore`, `FrameBuffer`,
  `PlaybackController`, `RenderCoordinator` (+ `FrameSink`), `ModelCacheController`,
  and the value/limit headers.

**Chunk 3.1 (Android JNI layer + handle safety) is complete**:
`android/src/main/cpp/` — `RlottieJni.cpp` (the narrow JNI surface),
`JniPlayerHandle` (the handle registry), `AndroidFrameSink` (poll-based),
`AndroidPixelConvert.h`, `AndroidEventEncoding.h`.

Two Android decisions worth knowing before touching that layer:

- **Handles are registry ids, not pointers.** The `jlong` indexes a
  mutex-guarded, never-reused id → owning-pointer map. A raw pointer + magic
  sentinel was tried and rejected: validating the sentinel after destroy *is* a
  use-after-free (ASan proves it), so it can't meet the "double handle calls
  fail safe" criterion.
- **The sink never calls into the JVM.** The render worker only records state;
  the UI thread drains it each Choreographer tick
  (`nativeHasNewFrame`/`nativeCopyFrontInto`/`nativePollEvent`). No `JavaVM`
  attach on the worker, no weak-global-ref lifetime hazard. The trade-off vs
  plan §3.1 is that the worker renders into the core's `FrameBuffer` and the UI
  thread blits (one pass, channel swap fused) instead of rlottie rendering
  directly into a locked Bitmap.

**Chunks 2.1, 2.2, and 3.2 are complete** (uncommitted):

- `ios/RNRlottiePlayer.h/.mm` (2.1) — the Obj-C++ adapter owning the coordinator.
- `ios/RNRlottieView.h/.mm` + `ios/RNRlottieFramePresenter.h/.mm` (2.2) —
  `CADisplayLink` via a weak `NSProxy` target, zero-copy `CGImage` over the
  front buffer, debounced resize.
- `android/src/main/java/com/rlottie/RlottieBridge.kt` + `RlottieView.kt` (3.2)
  — `@JvmStatic external fun` bindings (a plain `external fun` on a Kotlin
  `object` is an instance method and would NOT match the static JNI symbols),
  Choreographer tick, two-Bitmap swap.

Platform-specific trap worth remembering: **`View.getWidth()` is already
physical pixels** (do not multiply by `density`), whereas **UIView bounds are
points** (do multiply by `screenScale`). The plan's `dp * density` phrasing
refers to the conversion Android has already done for you.

**Chunks 2.3 and 3.3 are complete** — the bridge contract is now frozen in
`docs/bridge-contract.md`, which is the authority for both platforms and for
Phase 4. Read it before touching any prop, event, or command.
`ios/RNRlottieViewManager.mm` and `android/.../RlottieViewManager.kt` +
`RlottiePackage.kt` + `RlottieEvents.kt` implement it; verified identical on
component name, the eight command ids, the six event names, and all 13 props.

**Chunks 2.4 and 3.4 are complete** — the global `RlottieModule` (model-cache
size, clear, native version; all Promise-based on both platforms) and the source
resolvers: `ios/RNRlottieSourceResolver.h/.mm` + `android/.../RlottieSourceResolver.kt`.
The plan's file list omits an iOS resolver; that asymmetry was unintentional and
iOS has one, so both platforms accept and reject exactly the same sources. The
bundled-asset scheme is `asset:///…` on **both** platforms (see the contract doc
— each platform initially invented a different scheme).

**Chunk 3.5 is complete** — `android/build.gradle` now applies the Kotlin
plugin (without it, every `.kt` file was silently excluded and the AAR would have
shipped the `.so` with no classes), resolves the React Native artifact by reading
the installed version (`react-android` at RN ≥ 0.71, `react-native` before), and
excludes the duplicate `libc++_shared.so`. A standalone Gradle build produces a
real AAR containing both ABIs' `.so` plus all 11 Kotlin classes.

Two native faults that only a **real link** exposed — worth knowing before
touching `src/main/cpp/CMakeLists.txt`:

1. rlottie attaches its own linker version script (`rlottie.expmap`, ending in
   `local: *;`) as a PRIVATE link item. rlottie is static here, and CMake
   propagates a static lib's PRIVATE link items to consumers, so that script
   landed on our `.so` and hid **every** JNI entry point — the library would load
   and then fail at the first native call. We strip it from rlottie's interface
   and apply our own `react-native-rlottie.expmap`.
2. `armeabi-v7a` did not link at all: the NDK defines `__ARM_NEON__`, so rlottie
   compiled `vdrawhelper_neon.cpp` while its pixman NEON `.S` was never
   assembled. That `.S` is GNU-as syntax the NDK's integrated assembler rejects,
   and NDK r28 has no GNU as — so we undefine `__ARM_NEON__` for rlottie on that
   ABI (as the podspec already does on Apple arm64) and take the generic path.

Known follow-ups (not defects in the above):
- `resizeMode` and `cacheStrategy` are accepted and validated on both platforms
  but are **no-ops on Android** (no draw-path / cache-flag hook yet) — 3.4/3.5.
- `colorOverrides` discards alpha on both platforms (the core's `setColor` has
  no alpha parameter).
- `RlottieSourceResolver.kt` hardcodes the 16 MiB limit instead of reading
  `cpp/InputLimits.h` (iOS includes the header directly). Chunk 5.1 makes the
  limits configurable — wire Android to the real value then, or the two will
  silently drift.
- 3.4 now enforces app-private confinement on `{path}` and `resourcePath`, which
  3.3 passed through unvalidated. Any fixture pointing outside
  `filesDir`/`cacheDir`/`noBackupFilesDir` now gets `INVALID_SOURCE`.

**Chunk 4.1 is complete** — `src/types.ts` (the public TS surface),
`src/source.ts` (source normalization + cache keys), `src/sha256.ts`.

- The `RlottieErrorCode` union is now **reconciled with `cpp/ErrorCode.h`**:
  `INVALID_DIMENSIONS` and `RELEASED` added, `UNSUPPORTED_FEATURE` dropped
  (nothing emits it). `ErrorCode.h` stays the source of truth — change it first.
- Object sources are stringified **exactly once**, memoized on object identity
  in a `WeakMap`. A caller who passes a fresh object literal every render
  defeats that and re-stringifies + re-hashes on the JS thread.
- Cache keys are content-addressed (`sha256(json):callerCacheKey`), so a caller
  key can never alias two different payloads. SHA-256 is hand-rolled because the
  package has zero runtime deps and RN has no sync crypto.

**Chunks 4.2 and 4.3 are complete** — `src/RlottieNativeComponent.ts`,
`src/commands.ts`, `src/RlottieView.tsx`.

- **Command dispatch never hardcodes the numeric id.** It resolves via
  `UIManager.getViewManagerConfig('RlottieView').Commands[name]`, which
  `PaperUIManager.js` computes at runtime from the manager's real exported
  methods, and falls back to passing the command NAME string. Both avoid the
  iOS raw-array-index path, where a drifted integer is an `NSRangeException`
  that kills the app rather than a recoverable error. Do not "simplify" this to
  `RLOTTIE_COMMAND_IDS[name]` — that enum exists for reference, not dispatch.
- `dispatchRlottieCommand` never throws and returns `false` for an unmounted
  view, so `ref.play()` racing mount is safe.
- `RlottieView` reports a source-normalization failure as `onAnimationError`
  from an effect (never during render), deduped on the FAILURE CONTENT, not on
  the `source` object's identity — consumers pass inline `source={{uri}}`
  literals, so an identity guard would re-fire every render and any consumer
  setting state in that handler would spin.

**Chunk 4.4 is complete — Phase 4 is done.** `src/RlottieModule.ts`,
`src/index.ts`, `react-native.config.js`, and the `package.json` `exports` map.

- The native module is resolved lazily, per call. Throwing at import time would
  take down any consumer that merely imports `RlottieView` without a native
  runtime (a test file, storybook, a web build); failing at the call site keeps
  the blast radius to code that actually needs native.
- `src/index.ts` is the ONLY supported import path, and `exports` enforces it.
  `dispatchRlottieCommand` is deliberately NOT exported — a hand-rolled dispatch
  would bypass the unmounted-view guard and the id resolution that avoids the
  iOS raw-index path. `RlottieCommand` is exported for reference only.

Still not covered by tests: `RlottieView`'s render-time behaviour (the
error-dedup and stable-source-identity logic). `react-test-renderer` is now in
devDependencies but at **19.2.8, which cannot run against the pinned
`react@18.2.0`** (it declares `peerDependencies: react ^19.2.8` and crashes on
load reading React 19 internals). To close this gap, either pin
`react-test-renderer@^18.2.0` or move `react`/`@types/react` to 19 — note RN
0.81 itself expects React 19, so the devDependency pin at 18.2.0 is already out
of step with `react-native@0.81.0`.

**Phase 5 is complete** (5.1–5.5). Highlights and the traps worth knowing:

- **`rnrlottie::currentInputLimits()` / `setInputLimits()`** (`cpp/InputLimits.h`,
  header-only, mutex-guarded) is the ONE source of limits. Never construct
  `InputLimits{}` locally and never hardcode a copy — Android did the latter and
  it was the drift hazard 5.1 existed to kill. Android reads it over JNI
  (`nativeGetInputLimits`), iOS includes the header.
- **JSON nesting depth is a crash guard, not a policy knob.** rlottie's vendored
  rapidjson is recursive-descent, so deep input exhausts the native stack and
  kills the process — unrecoverable, no typed error possible. Measured: depth
  100,000 is only ~1.9 MB (11% of the byte limit) and reproducibly SIGSEGV'd an
  8 MiB stack; the render worker's stack is far smaller. `checkJsonDepth()` now
  rejects over-deep JSON before rlottie sees it. **A byte limit does NOT bound
  nesting** — the nesting token is ~18 bytes and a bare `[` is one.
- Cache keys: `sha256(json):callerKey:rlottieCommit:v1:resourcePath`, composed in
  `cpp/CacheKey.h`. The hash is recomputed natively over the real bytes and sits
  first, so a caller-supplied key can never alias two payloads.
- `tests/run-tests.sh fuzz` is an opt-in bounded smoke run, not part of the gate.

Known gaps, deliberately not closed:

- **The depth guard covers the Data path only.** `loadFromFile` hands a path
  straight to rlottie, which reads and parses the bytes itself, so a hostile
  file on disk still reaches the recursive parser. Closing it means the core
  reading the file itself — a change to *how* File sources load. The resolvers
  confine file sources to app-private storage, which narrows but does not
  eliminate the vector (a `content://` copy or downloaded asset lands there).
- **Real libFuzzer never ran here**: this machine's Xcode clang ships no
  `libclang_rt.fuzzer_osx.a`. `run-tests.sh fuzz` detects the link failure and
  falls back to an ASan/UBSan standalone driver (~19k iterations, no findings).
  That proves the harness works; it is NOT coverage-guided fuzzing. On a machine
  with full LLVM the same command gets the real engine, no code change.
- `maxExternalAssets`/`maxExternalBytes` are enforced by the resolvers over the
  asset directory, but not by the core — rlottie exposes no pre-parse API to
  enumerate embedded assets.
- File-backed cache keys remain location-derived (`rlottie::loadFromFile` takes
  no key), so two different files at one path collide. Documented in
  `cpp/CacheKey.h` and `src/source.ts`.

**Phase 6 is complete** (6.1 dynamic properties, 6.2 marker playback).

- `opacityOverrides` / `strokeWidthOverrides` join `colorOverrides`, and
  `playMarker` is **command id 9**. Commands are now **append-only**: on iOS the
  integer id is a raw declaration index, so a new command must be declared LAST
  in `RNRlottieViewManager.mm`. Current order (verify after any edit):
  `__reservedCommandSlot0 play pause resume stop reset seekToProgress
  seekToFrame setSpeed playMarker`.
- All three override setters route through `RenderCoordinator`'s control queue
  and are applied on the render worker — never from the caller thread (plan §6).
- `play({marker})` wins over `startFrame`/`endFrame` rather than merging.

**A real bug was found and fixed here**: `PlaybackController::play(start, end)`
was persisting its supposedly one-off range into `config_`, so a later
argument-less `play()` inherited it — contradicting what the contract already
claimed and what the JNI/Obj-C plumbing appeared to guarantee. The `-1`→
`nullopt` marshalling was correct all along; the leak was one layer deeper, in
the controller. `play()` now re-derives from the persistent config via
`resolveSegment()` before applying an override. Both no-leak cases are
regression-tested (`playback_*_is_one_off_does_not_leak*`).

Known limitations (documented at the call sites, not defects):

- `setOpacity` uses rlottie's `FillOpacity`, so a **stroke-only shape's opacity
  override has no effect** — rlottie has no single fill+stroke opacity property,
  and `setColor` already had the same fill-only scope.
- This vendored rlottie skips re-evaluating a layer's paint on repeat renders
  when the layer has no animated keyframes, so a `setValue()` override added
  afterwards may not take effect. Pre-existing engine behaviour that equally
  affects the shipped `setColor`; noted in `tests/cpp/core_tests.cpp`.

Next: **Phase 7** (performance & stability).

Not done, and out of reach in a headless environment: the plan's "example app
runs on device + emulator" for Chunk 3.5. `example/` is still an empty
placeholder — building it is Phase 4/5 work and needs a real device or emulator.

### Commands

```bash
# JS/TS (needs `npm install` first)
npm run typecheck        # tsc --noEmit
npm run test:js          # source normalization + cache-key tests (no framework)
npm run lint             # eslint src
npm run format:check     # prettier --check  (NOTE: several .md files at repo
                         # root were already unformatted before Phase 4)

# Shared C++ core tests — plain + ASan/UBSan + TSan variants.
# Uses CMake+CTest when available; otherwise a direct clang fallback.
tests/run-tests.sh                 # all variants
tests/run-tests.sh plain           # a single variant (plain|asan|tsan)

# Android native build checks.
# Syntax-only (fast): type-checks each JNI TU per ABI.
scripts/check-android-build.sh [<api-level>]
# Real link (slower, stronger): builds libreact-native-rlottie.so per ABI and
# asserts the Java_com_rlottie_* symbols are actually EXPORTED. Use this before
# trusting native changes — a syntax-only pass cannot see link or
# symbol-visibility faults (see the Chunk 3.5 note below).
scripts/check-android-build.sh --link [<api-level>]

# Re-vendor / bump the pinned rlottie revision
scripts/update-rlottie.sh [<commit-sha>]
```

Native iOS/Android device builds require Xcode / the Android NDK (not present in
every dev environment); Chunk 0.3 was verified via an equivalent host compile.

## What is being built

`react-native-rlottie` — a React Native library that renders Lottie animations
using the **rlottie** C++ engine. Read
`react-native-rlottie-implementation-plan.md` in full before doing any
implementation work; it is the source of truth for architecture, phases, APIs,
and acceptance criteria. Key framing:

- **Target:** React Native Legacy Architecture / Bridge, RN ≤ 0.81 with
  `newArchEnabled=false`. Fabric/TurboModules are out of scope for v1, but the
  layering is deliberately designed so a future Fabric adapter can reuse the
  core.
- **It is a native UI component**, not a native module that streams frames over
  the bridge.

## Architecture: the three-layer boundary

The single most important structural rule in the plan is the separation between
three layers. Preserve this boundary in every change:

1. **Stable TypeScript API** (`src/`) — `RlottieView` + imperative ref. Hides
   `UIManager.dispatchViewManagerCommand` and `findNodeHandle`; consumers never
   touch platform methods.
2. **Legacy bridge adapters** — iOS `RCTViewManager` + Objective-C++ (`ios/`),
   Android `SimpleViewManager` + JNI (`android/`). Thin; no business logic.
3. **Shared C++ core** (`cpp/`) — `RlottiePlayerCore`, a platform-neutral façade
   over vendored rlottie. Owns parsing, timing, frame selection, rendering,
   buffers, caching, and cleanup.

### Non-negotiable invariants (from the plan)

- **No per-frame JS↔native bridge traffic.** JS sends only source changes,
  playback config, imperative commands, and infrequent lifecycle events. No
  `onFrame` event; no frame pixels cross the bridge.
- **The animation object is never rendered and mutated concurrently.** All
  access to a given `rlottie::Animation` is serialized on one render worker.
- **Generation-based cancellation.** An `std::atomic<uint64_t>` generation is
  bumped on source/dimension change, detach, reset, and destruction; renders
  capture the generation and stale results are discarded rather than presented.
- **Single in-flight render per view, latest-frame-wins.** Never accumulate a
  render queue.
- **Reusable double buffers**, allocated only after layout dimensions are known,
  with overflow-checked sizing and a clamped max pixel area. No per-frame
  allocation.
- **No exceptions across JNI/Objective-C++ boundaries.** Return typed result
  codes (`PlayerErrorCode`), converted to one consistent React event format.
- **Untrusted input.** Lottie JSON is parsed by native C++; enforce byte/size/
  dimension limits, canonicalize resource paths, disallow filesystem escape, and
  use a pinned, patched rlottie commit (vendored, never fetched at build time).

Frame scheduling uses platform display clocks (iOS `CADisplayLink`, Android
`Choreographer`) with frames derived from a monotonic clock — see plan §5.

## Implementation order

Follow the phases in plan §19. **Phase 0 (compatibility lock + pixel-format
spike) gates everything** — verify rlottie's ARGB32-premultiplied output,
channel order, and stride render correctly on both platforms before building
real playback (§8).

## Specialized agents

Delegate to the agents in `.claude/agents/` according to their domain:

- **CPPArchitect** (`opus`) — high-level C++/system design: boundaries,
  contracts, ownership/threading models, API/ABI. Decides *how* things are
  structured; hands the design to CPPEngineer.
- **CPPEngineer** (`sonnet`) — implements the shared C++ core, JNI/Objective-C++
  glue, native rendering, CMake/podspec, and C++ tests.
- **RNEngineer** (`sonnet`) — the TypeScript API, `RlottieView`, bridge adapters
  (ViewManagers, commands, events), and packaging/autolinking.

Natural workflow: CPPArchitect designs the core seams → CPPEngineer implements
the native side → RNEngineer builds the JS-facing layer and bridge glue. The
C++ agents defer JS/bridge work to RNEngineer, and RNEngineer defers engine
internals to the C++ agents.
