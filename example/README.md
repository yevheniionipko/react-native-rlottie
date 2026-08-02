# react-native-rlottie example app

A real React Native app (RN 0.81.0, generated from the standard community
template) that consumes `react-native-rlottie` as a normal local dependency —
`"react-native-rlottie": "file:.."` in `package.json` — resolved through
autolinking, exactly like an app that installed the package from npm would.

It is **not** a hello-world: one screen exercises the real surface —

- Two `RlottieView`s: one from a bundled `asset:///` source
  (`android/app/src/main/assets/example-lottie.json` +
  `ios/RlottieExample/example-lottie.json`, added as an Xcode "Copy Bundle
  Resources" entry), one from an inline `{json: <object>}` source
  (`assets/example-lottie.json` via `require`, hoisted to module scope so it is
  stringified/hashed exactly once — see `src/types.ts`'s warning about fresh
  object literals).
- Every imperative ref command: `play` (with and without a one-off frame
  range), `pause`, `resume`, `stop`, `reset`, `seekToProgress`, `seekToFrame`,
  `setSpeed`, and `play({marker})` (buttons appear once `onAnimationLoaded`
  reports markers).
- The playback-shaping props: `loop`, `autoPlay`, `speed`, and a `resizeMode`
  cycle button (`contain` → `cover` → `stretch` → `center`).
- The three dynamic-property override arrays: `colorOverrides`,
  `opacityOverrides`, `strokeWidthOverrides`, toggleable with an editable
  `keyPath`.
- `metricsEnabled` + the `onMetrics` payload, rendered live.
- All six lifecycle/error events (`onAnimationLoaded`, `onAnimationError`,
  `onAnimationStart`, `onAnimationPause`, `onAnimationLoop`,
  `onAnimationFinish`) into a scrolling event log.
- The global `Rlottie` module: `getNativeVersion()`, `clearModelCache()`,
  `configure({modelCacheSize})`, `isAvailable()`.

## Requirements this app is pinned to

**Legacy Architecture — `newArchEnabled=false` is required.** This library
targets RN's Legacy Bridge only (RN ≤ 0.81); Fabric/TurboModules are v2+
scope (root `CLAUDE.md`). This example is deliberately configured accordingly:

- `android/gradle.properties` sets `newArchEnabled=false` (the RN 0.81
  template default is `true`).
- `ios/Podfile`'s `use_react_native!` call passes `fabric_enabled: false,
  new_arch_enabled: false` explicitly, so it stays off even if
  `RCT_NEW_ARCH_ENABLED=1` is set in the shell running `pod install`.

Do not flip either of these — an app defaulting to the new architecture would
not exercise the shipped native code at all.

## Install

From this directory:

```bash
npm install
```

This resolves `react-native-rlottie` from the parent directory (`file:..`) and
autolinking picks it up automatically — no manual native linking needed.

`metro.config.js` adds the repo root to `watchFolders` (the library's real
source lives one level up, outside this app's own `node_modules`) and forces
`react`/`react-native` to resolve to *this app's* copies even when required
from a file physically under the repo root — otherwise Metro would find the
repo root's own (differently-versioned) `node_modules/react`, load two React
copies, and crash with "Invalid hook call" the moment `RlottieView`'s hooks run.

### iOS — install CocoaPods dependencies

```bash
cd ios
bundle install   # once
bundle exec pod install
cd ..
```

## Run

With an Android emulator booted, or a device connected:

```bash
npx react-native run-android
```

With an iOS Simulator booted (or `xcrun simctl boot <udid>`):

```bash
npx react-native run-ios
```

Or open `ios/RlottieExample.xcworkspace` in Xcode and hit Run.

## Verifying it actually rendered

A green build with a blank view is the failure mode that matters for this
library — bridge wiring can compile and link correctly while the render path
is silently a no-op. `example/screenshots/android.png` and
`example/screenshots/ios.png` are real captures from a booted emulator/
simulator (`adb exec-out screencap -p` / `xcrun simctl io <udid> screenshot`)
showing the bundled animation actually playing, not an empty placeholder.
