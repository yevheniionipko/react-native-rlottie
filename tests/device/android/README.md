# Chunk 7.4 (Part C) — Android on-device golden verification

Files here are consumed by exactly one thing: `scripts/run-golden-android.sh`.
Nothing in this directory is picked up by `tests/run-tests.sh` or CMake — this
is a device leg, not part of the host gate.

- `golden_runner.cpp` — a plain native Android executable (no JNI, no APK, no
  gradle). Cross-compiled with the NDK, pushed to `/data/local/tmp`, and run
  directly via `adb shell`. It #includes `tests/cpp/GoldenFixtures.h` and
  `tests/cpp/GoldenCompare.h` (Part A) so the fixture list and comparison
  tolerances can never drift from the host gate's copy — it does not
  duplicate them. Runs two legs:
  1. **core** — `RlottiePlayerCore::loadFromData` + `renderFrame` for every
     golden frame in the table, byte-exact against the committed
     `tests/golden/*.argb32.raw`.
  2. **conversion** — the committed golden bytes through the shipped
     `rlottieSurfaceToAndroid()` (`android/src/main/cpp/AndroidPixelConvert.h`)
     into a padded-stride destination, asserting exact R,G,B,A channel
     placement, untouched stride padding, and a lossless swap-back round
     trip. Also writes a tightly-packed RGBA copy of every frame plus a
     `manifest.txt` for Leg 3 to consume.
- `BitmapGoldenMain.java` — Leg 3. Run under `app_process` (no gradle, no
  APK): reads the manifest Leg 2 wrote, loads each RGBA file into a REAL
  `android.graphics.Bitmap` via `copyPixelsFromBuffer`, and checks
  `getPixel()`. This is the step that turns
  `docs/pixel-format-report.md`'s Android byte-order claim into an actual
  on-device measurement rather than documentation. See the tolerance
  discussion in that file's header comment before touching the bounds —
  opaque pixels are checked byte-exact; only alpha-blended pixels get a
  small, explicitly-justified rounding allowance (Skia's un-premultiply
  divide), never used to paper over a real channel-order bug.

## Running

```bash
scripts/run-golden-android.sh
```

Requires a booted device/emulator visible to `adb devices`, the Android NDK,
and `platform-tools`/`build-tools`/a `platforms/android-*` SDK component. Fails
loudly (non-zero exit, no silent skip) if any of those are missing — see that
script's header comment for the exact precedent this follows
(`tests/run-tests.sh`'s `leaks` variant).

## Traps worth knowing (hit while building this)

- **zsh does not word-split an unquoted `$VAR`.** A space-separated string of
  `-I` flags collapses into a single argument under zsh and every include path
  silently fails to resolve. The build script uses bash arrays throughout
  (`"${INC[@]}"`) and is invoked via its `#!/usr/bin/env bash` shebang, not
  sourced into a zsh shell.
- **`d8` needs every `.class` file for a class with nested types, not just the
  top-level one.** `BitmapGoldenMain$ManifestEntry` is a separate `.class`
  file that shares a "nest" with `BitmapGoldenMain`; passing only
  `BitmapGoldenMain.class` to `d8` fails with "requires its nest mates
  ... to be on program or class path". Pass the whole `classes/*.class` glob.
- **No `-lpthread` and no pushed `libc++_shared.so` needed.** Bionic's libc
  already exports pthread symbols, and `-static-libstdc++` statically links
  the NDK's libc++ into the executable (there is no libstdc++ on Android; the
  NDK compiler driver maps the flag onto libc++), so `golden_runner` is a
  single self-contained ELF — verified by running it directly via
  `adb shell` with no extra libraries pushed.
- **The NDK API used to *compile* (`RNRLOTTIE_NDK_API`, default 24) is
  independent of the device's actual API level.** It only sets the binary's
  minimum-API floor; a lower compile-time API than the device's own is
  expected and fine (verified: compiled for API 24, ran on an API 37 device).
  Likewise `android-36/android.jar` (the highest `platforms/` component
  installed when this was written) was used for `javac`/`d8` even though the
  device reports API 37 — an `android.jar`'s version only bounds which SDK
  *symbols* are available at compile time, not which device the resulting
  `.dex` can execute on.
