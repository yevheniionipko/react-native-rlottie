# Golden verification (Chunk 7.4)

**Status: PASSING on both platforms, byte-exact.** This document is the
authority for what "golden frames verified on-device on both platforms with
permitted-conversion tolerance" (plan §19 Phase 7, detailed plan chunk 7.4)
actually means here — what was measured, on what, and what was _not_.

Read this before adding a fixture, changing a tolerance, or regenerating a
golden.

## 1. What is verified, and where

Three code paths sit between "a Lottie file" and "pixels on screen", and each
is verified in the place where it can actually fail:

| Leg                    | What it proves                                                                                                                         | Runner                                                             | Where it runs                         |
| ---------------------- | -------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------ | ------------------------------------- |
| **Core**               | rlottie + `RlottiePlayerCore`/`FrameBuffer` produce the committed golden bytes                                                         | `tests/cpp/golden_tests.cpp`; both device runners                  | host, iOS Simulator, Android emulator |
| **iOS conversion**     | the shipped `ios/RNRlottieFramePresenter.mm` turns a front buffer into a `CGImage` whose pixels survive a real CoreGraphics round-trip | `tests/device/ios/golden_runner.mm`                                | iOS Simulator                         |
| **Android conversion** | the shipped `rlottieSurfaceToAndroid()` lands R,G,B,A correctly, and a **real `android.graphics.Bitmap`** agrees                       | `tests/device/android/golden_runner.cpp` + `BitmapGoldenMain.java` | Android emulator                      |

The one fixture table (`tests/cpp/GoldenFixtures.h`) and the one comparator
(`tests/cpp/GoldenCompare.h`) are `#include`d by all three runners. Neither is
duplicated per platform — that drift hazard is the same one CLAUDE.md already
records for `InputLimits.h`, and it is the reason both headers are
dependency-free (std only, no rlottie, no test harness) so an NDK or
`iphonesimulator`-SDK build can consume them unchanged.

## 2. Commands

```bash
tests/run-tests.sh                  # host golden tests are in the DEFAULT gate
tests/run-tests.sh golden-gen       # OPT-IN: regenerate tests/golden/*.raw
scripts/run-golden-ios.sh           # OPT-IN: iOS Simulator, needs Xcode + a simulator
scripts/run-golden-android.sh       # OPT-IN: Android, needs the NDK + an adb device
```

The two device scripts are deliberately **not** in the default gate — they need
hardware/an emulator that not every environment has. Neither skips silently: if
the SDK, NDK, or a device is missing they fail loudly, following
`tests/run-tests.sh`'s `leaks`-variant precedent.

## 3. Fixture corpus

`tests/fixtures/golden/` — six hand-written 64×64, 31-frame (30fps) fixtures;
20 golden frames total.

| Fixture        | Covers                                                           | Golden frames | Measured non-transparent / distinct colors (worst frame) |
| -------------- | ---------------------------------------------------------------- | ------------- | -------------------------------------------------------- |
| `shapes`       | rects/ellipses/paths, fills **and** strokes, animated transform  | 0, 10, 20, 30 | 33.1% / 123                                              |
| `masks`        | a `masksProperties` mask **and** a `tt:1` alpha track-matte pair | 0, 15, 30     | 48.0% / 55                                               |
| `gradients`    | linear + radial gradient fills                                   | 0, 15, 30     | 100% / 114                                               |
| `transparency` | overlapping partial-alpha layers (premultiplication)             | 0, 15, 30     | 59.8% / 8                                                |
| `precomp`      | a precomposition layer referencing an asset comp                 | 0, 15, 30     | 35.5% / 30                                               |
| `markers`      | several named markers over a moving shape                        | 0, 10, 25, 30 | 11.7% / 23                                               |

### Non-vacuity is enforced, per fixture

A syntactically-valid Lottie that rlottie renders as **fully transparent** makes
a golden gate pass vacuously: an empty render matches an empty golden, and every
renderer bug that also produces blankness passes with it. So each fixture pins a
`minNonTransparentFraction` / `minDistinctColors` floor (`GoldenFixtures.h`),
measured with margin, checked by both the host test and the generator.

The floors are per-fixture rather than a global `> 0%` on purpose. The byte-exact
check only protects a fixture _as long as nobody regenerates the goldens_ —
regeneration is exactly the moment a fixture that has rotted into near-blankness
would be blessed into a new golden that agrees with it. A global `> 0%` floor
waves that through; a pinned floor fails it.

**This trap is not hypothetical.** Building `masks.json` surfaced that
`masksProperties` alone does _nothing_ unless the layer also sets
`"hasMask": true` — rlottie's `renderer::Layer::Layer` only builds a `LayerMask`
when `mLayerData->mHasMask`, and the parser sets that flag from the separate
`"hasMask"` key. Without it the fixture rendered an opaque canvas with no mask
applied at all, while still being valid JSON that "loaded fine".

### Documented gap: text and image fixtures

Plan §20 also lists text and image fixtures. Both need external resources (a
bundled font; an embedded or on-disk raster asset) this corpus does not ship, and
a fixture whose resource fails to resolve renders as blank/missing content —
precisely the vacuity trap above. So text/image golden coverage is an explicit
gap, not a fixture that would quietly pass by rendering nothing.

## 4. Tolerance policy

Two legs, two regimes, and the split is deliberate:

- **The core leg is byte-exact (0 delta, 0 differing pixels).** rlottie is the
  same C++ on both platforms; nothing sits between "core renders" and "here are
  the ARGB32 bytes". If this leg ever needed tolerance it would mean the render
  is nondeterministic across platforms — a correctness bug to fix, not a golden
  to loosen.
- **The platform-conversion leg is allowed tolerance** (`ToleranceOptions`: a max
  per-channel delta **and** a max fraction of differing pixels), because a real
  `CGImage`/`Bitmap` readback can perturb channel values. Bounding both numbers
  matters: a systematic corruption (transposed channels, a stuck row) still
  fails loudly instead of hiding under a generous global average.

**What the tolerance actually turned out to be:**

- **iOS: 0/0 — no tolerance needed, and none was added.** All 100 checks are
  byte-exact, including through a deliberately padded-stride front buffer.
- **Android native conversion: 0/0**, byte-exact, including untouched stride
  padding and a lossless swap-back round-trip.
- **Real-`Bitmap` leg: opaque pixels exact; blended pixels ±2, of which ≤1 was
  ever used.** `Bitmap.getPixel()` returns a _straight_ color, so Skia divides
  by alpha internally, and this file's reference un-premultiply rounds
  independently — two roundings of one premultiplied byte can land a step apart.
  Measured worst delta across the whole corpus: **1**, on `shapes` frame 10 only;
  every other frame exact. The allowance is 2 (one step of headroom for a
  different Skia version) and every frame prints
  `worstBlendedDelta=<observed>/<allowed>` so it stays evidenced rather than
  inherited on trust. A channel-order bug would show deltas of order 100+, not 1.

## 5. Results

Verified 2026-08-02 on this machine.

|         | Environment                                                                           | Result                                                          |
| ------- | ------------------------------------------------------------------------------------- | --------------------------------------------------------------- |
| Host    | Apple clang 21.0.0, arm64 macOS                                                       | `ALL 107 TESTS PASSED` × plain/asan/tsan (7 of them `golden_*`) |
| iOS     | iPhone 16 Pro simulator, **iOS 18.2**, Xcode 26.6 (17F113)                            | **100/100 checks PASS**, byte-exact                             |
| Android | `sdk_gphone16k_arm64` emulator, **Android 17 / API 37, arm64-v8a**, NDK 28.0.12433566 | **160/160 native checks PASS** + Leg 3 **20/20 frames PASS**    |

The Android run also confirms `docs/pixel-format-report.md` §4's Android claim by
**measurement** rather than documentation: bytes `FF,00,00,FF` in an ARGB_8888
`Bitmap` read back as `getPixel() == 0xFFFF0000`, and `00,00,FF,FF` as
`0xFF0000FF`. Until this chunk, `kAndroidNeedsChannelSwap = true` rested on
Android's documented format alone.

## 6. What is NOT verified

Be precise about this — the chunk's contract says "on-device" and these ran on a
**simulator and an emulator**, which is not the same thing.

- **No physical hardware.** Both runs are virtual devices on a development Mac.
  The core leg is CPU-only rendering, so a physical device is unlikely to differ,
  but "unlikely" is not "measured".
- **The Android scripts work unchanged on a physical device** — `adb shell` and
  `app_process` behave the same, and the ABI is read from
  `ro.product.cpu.abi` rather than hardcoded. Plug a phone in and rerun.
- **The iOS script does NOT.** `simctl spawn` is simulator-only; running this on
  a real iPhone needs a signed app/XCTest bundle as a test host, which is not
  implemented here. That is the single biggest remaining gap in this chunk.
- **No GPU/compositing path.** The iOS leg reads pixels back out of a
  `CGBitmapContext`, not off the screen; nothing here verifies what
  `CALayer`/`UIView` or Android's view compositor finally displays (color
  management, scaling, `resizeMode`). A screenshot-based comparison is what would
  cover that.
- **No text or image fixtures** — see §3.
- **One ABI each.** arm64 only (simulator arm64, emulator arm64-v8a). armeabi-v7a
  and x86_64 are covered by `scripts/check-android-build.sh --link` for _linkage_,
  not for golden pixels.
