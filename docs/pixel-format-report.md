# Pixel-format report (Chunk 0.4 — Phase 0 gate)

**Status: RESOLVED.** This report is the gate that unblocks the platform
presenters (Chunks 2.2 iOS, 3.1/3.2 Android). Do not implement a presenter
without conforming to the constants in [§4](#4-constants-for-downstream-chunks).

## 1. Method

rlottie's `Surface` pixel format is decided by the rlottie renderer, **not** by
the host platform — it is the same C++ code on iOS and Android. So the
authoritative format is determined once, by rendering a known fixture with the
vendored rlottie and inspecting the raw bytes. The platform layers only
*reinterpret* those bytes.

- **Fixture:** `tests/fixtures/pixel-probe.json` — a 64×64 comp with opaque
  red / green / blue quadrants, a 50%-opacity red quadrant, and a transparent
  center cross.
- **Harness:** `cpp/spike/pixel_probe.cpp` (shared, platform-neutral). Renders
  frame 0 into a caller-provided `uint32_t` buffer via `renderSync` and dumps
  each sample pixel's 32-bit word plus its 4 memory bytes in **address order**
  (`mem[0]` = lowest address = little-endian LSB).
- **Verified on:** Apple clang, arm64 (host). Because rlottie's output is
  platform-independent and all target ABIs (arm64-v8a, x86_64, armeabi-v7a) are
  little-endian, the result holds on both platforms. Reproduce with the build in
  Chunk 0.3's notes, then `pixel_probe tests/fixtures/pixel-probe.json out.raw`.
- **Golden buffer:** `tests/golden/pixel-probe-frame0.argb32.raw` (16384 bytes,
  raw ARGB32 as emitted by rlottie).

## 2. Raw evidence

```
rlottie totalFrame=31 duration=1.000  surface=64x64 stride=256 bytes
red   (10,10)    word=0xFFFF0000  mem[0..3]=00 00 FF FF   B=00 G=00 R=FF A=FF
green (54,10)    word=0xFF00FF00  mem[0..3]=00 FF 00 FF   B=00 G=FF R=00 A=FF
blue  (10,54)    word=0xFF0000FF  mem[0..3]=FF 00 00 FF   B=FF G=00 R=00 A=FF
red50 (54,54)    word=0x7F7F0000  mem[0..3]=00 00 7F 7F   R=7F(127) A=7F(127)
clear (32,32)    word=0x00000000  mem[0..3]=00 00 00 00   fully transparent
```

## 3. Findings

| Property | Result | Evidence |
|---|---|---|
| **Channel / byte order** | **B, G, R, A** in memory (byte[0]=Blue … byte[3]=Alpha). On little-endian this is the 32-bit word `0xAARRGGBB`, i.e. **ARGB32**. | R/G/B quadrants land in bytes 2/1/0 respectively. |
| **Premultiplied alpha** | **Yes.** 50%-opacity red → `R=127, A=127` (not `R=255`). Transparent → all-zero `(0,0,0,0)`. | red50 word `0x7F7F0000`. |
| **Row orientation** | **Top-down** (row 0 = top of comp). No vertical flip needed. | red layer (top) at low rows; blue layer (bottom) at high rows. |
| **Stride** | Equals the caller-provided `bytesPerLine`; rlottie honors it. For width 64 → 256 bytes, no padding. Presenters MUST pass their surface's real stride (may be padded). | `surface.bytesPerLine()` = 256. |
| **Color space** | sRGB, straight component values (pre multiply aside). No ICC transform applied by rlottie. | Pure channel values round-trip exactly. |

> ⚠️ **Do not assume the 32-bit buffer is passable unchanged to every native
> bitmap API.** It matches Core Graphics with the right flags but requires an
> **R↔B swap** for Android `ARGB_8888` (see below).

## 4. Constants for downstream chunks

### `kCoreSurfaceFormat`
`ARGB32_Premultiplied` — memory byte order **B,G,R,A**; little-endian word
`0xAARRGGBB`; top-down rows; stride = surface `bytesPerLine`.

### iOS — `kIOSBitmapInfo` (Chunk 2.2)
```
CGBitmapInfo = kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little
colorSpace   = CGColorSpaceCreateDeviceRGB()   // sRGB; reuse one instance
bitsPerComponent = 8, bitsPerPixel = 32
```
`AlphaPremultipliedFirst` puts A in the word's most-significant byte
(`0xAARRGGBB`); `ByteOrder32Little` reads the word little-endian → memory bytes
land as **B,G,R,A**, exactly matching rlottie. **No channel swap on iOS.**

### Android — `kAndroidNeedsChannelSwap = true` (Chunks 3.1 / 3.2)
`Bitmap.Config.ARGB_8888` in native memory (`AndroidBitmap_lockPixels`,
`ANDROID_BITMAP_FORMAT_RGBA_8888`) is byte order **R,G,B,A**, whereas rlottie
emits **B,G,R,A**. Therefore the red and blue channels must be swapped when
copying/rendering into the bitmap. Premultiplication already matches
(`ARGB_8888` is premultiplied by default), so **only R↔B differs** — no alpha
re-multiplication. On little-endian this is a swap of byte[0]↔byte[2] per pixel
(or a `0x00RRGGBB`-lane shuffle). Keep this on the render worker (Chunk 3.1); it
is the one measurable per-pixel cost of the Android path and a candidate for
SIMD in Phase 7.

## 5. Acceptance

- [x] Fixture renders and every sampled region is pixel-semantically correct.
- [x] Channel order, premultiplication, stride, and row orientation determined
      from raw bytes (not assumed).
- [x] Exact per-platform interpretation specified (iOS bitmap info; Android R↔B
      swap) so both presenters are deterministic.
- [x] Golden buffer committed for regression (`tests/golden/`).

**Gate open:** presenters may now be implemented against §4. The final on-device
*visual* confirmation (feeding this buffer through `CGImage` / `Bitmap`) happens
when those presenters exist (2.2 / 3.2); this report makes that step
mechanical rather than exploratory.
