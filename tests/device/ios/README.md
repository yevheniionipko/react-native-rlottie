# Chunk 7.4 (Part B) — iOS-simulator golden runner

`golden_runner.mm` is a standalone Objective-C++ binary, built and executed
directly against a booted iOS Simulator via `scripts/run-golden-ios.sh`. It is
**not** part of `tests/run-tests.sh` (that suite never leaves the host clang
toolchain) and it is **not** an Xcode/XCTest target — there is no app bundle,
no test host, just a plain executable `simctl spawn`s on the simulator's own
filesystem.

## Why this exists

`tests/cpp/golden_tests.cpp` (Part A) proves rlottie renders the committed
goldens byte-exact on the **host** machine's clang/arm64. It says nothing
about the iOS Simulator runtime, nor about the real
`ios/RNRlottieFramePresenter.mm` — the code that actually turns a
`FrameBuffer` front buffer into what a view presents. This runner closes both
gaps by actually executing on-device (simulator) code:

- **Core leg**: `RlottiePlayerCore`/`FrameBuffer` render the same golden
  frames on the simulator and are compared byte-exact against the same
  `tests/golden/*.raw` files Part A already checks.
- **Platform-conversion leg**: the real, shipped
  `ios/RNRlottieFramePresenter.mm` (compiled straight into this binary, not
  reimplemented) builds a `CGImageRef`, which is drawn into a
  `CGBitmapContext` using the exact format from
  `docs/pixel-format-report.md` §4 and read back for comparison. Run twice per
  frame — once over the `FrameBuffer`'s own (always tightly-packed) front
  buffer, once over a synthetic, deliberately-padded-stride `FrontBuffer` — so
  a stride bug in `CGImageCreate`'s `bytesPerRow` handling would be caught
  even though nothing in this codebase today produces a padded real one.

See `golden_runner.mm`'s own header comment for the full design, the
tolerance policy (byte-exact, verified to hold — see the RESULT note there),
and the one real bug this exercise found (in the test harness, not the
presenter: the readback context needed `kCGBlendModeCopy`, not the default
source-over blend, or transparent source pixels would leave the pre-draw
poison bytes visible).

## Running it

```bash
scripts/run-golden-ios.sh                # auto-discovers/boots a simulator
scripts/run-golden-ios.sh --udid <UDID>  # use a specific one
```

Fails loudly (non-zero exit, explicit stderr message) if Xcode / the
iphonesimulator SDK / any simulator runtime isn't available — it does not
silently skip, matching `tests/run-tests.sh`'s `leaks` variant precedent.

Compiled objects for vendored rlottie are cached under `build/ios-golden/`
(same pattern as `tests/run-tests.sh`), so re-runs after the first are a few
seconds. Delete `build/ios-golden/` to force a clean rebuild (e.g. after
bumping the pinned rlottie commit).

## What this does NOT cover

- Real device hardware (this is simulator-only; Chunk 7.4's device leg is a
  separate, still-open item per CLAUDE.md).
- Anything through `RNRlottieView`/`RNRlottiePlayer` (the actual view/adapter
  layer) — this runner only exercises `RlottiePlayerCore`, `FrameBuffer`, and
  `RNRlottieFramePresenter` directly, not the full Objective-C++ adapter or
  `CADisplayLink` scheduling.
- Text/image fixtures — out of scope for the whole golden corpus (see
  `tests/cpp/GoldenFixtures.h`'s header comment), not something Part B adds.
