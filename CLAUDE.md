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

Next: Phase 2 (iOS) and Phase 3 (Android) — the platform adapters — which can
proceed in parallel now that the core (through Chunk 1.5) exists.

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
