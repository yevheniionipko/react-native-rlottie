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

Known follow-ups (not defects in the above):

- `android/build.gradle` has no `kotlin-android` plugin or Kotlin sourceSet, so
  the Kotlin is not yet part of the library build — **Chunk 3.5**.
- `resizeMode` and `cacheStrategy` are accepted and validated on both platforms
  but are **no-ops on Android** (no draw-path / cache-flag hook yet) — 3.4/3.5.
- `colorOverrides` discards alpha on both platforms (the core's `setColor` has
  no alpha parameter).

Next: Chunks 2.4/3.4 (global config module + source resolvers), 3.5 (Gradle),
then Phase 4.

### Commands

```bash
# JS/TS (needs `npm install` first)
npm run typecheck        # tsc --noEmit
npm run lint             # eslint src
npm run format:check     # prettier --check

# Shared C++ core tests — plain + ASan/UBSan + TSan variants.
# Uses CMake+CTest when available; otherwise a direct clang fallback.
tests/run-tests.sh                 # all variants
tests/run-tests.sh plain           # a single variant (plain|asan|tsan)

# Type-check the JNI sources against the real NDK headers, per ABI.
# (RlottieJni.cpp needs <jni.h>/<android/bitmap.h>, so run-tests.sh can't build
#  it; the tests cover the JNI-free logic those entry points call.)
scripts/check-android-build.sh [<api-level>]

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
