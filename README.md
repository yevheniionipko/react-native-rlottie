# react-native-rlottie

A React Native **native UI component** that renders Lottie animations using the
vendored [rlottie](https://github.com/Samsung/rlottie) C++ engine. Parsing,
timing, frame selection, rendering, and buffer management all happen in native
code (`cpp/`); JS only sends the animation source, playback configuration, and
imperative commands, and receives infrequent lifecycle events back.

**No per-frame bridge traffic.** There is no `onFrame` event and no frame
pixel data ever crosses the JS↔native bridge — each platform draws directly
from a native frame buffer on its own display clock (`CADisplayLink` on iOS,
`Choreographer` on Android). The only recurring event is the opt-in
`onMetrics`, throttled to at most once per second (see
[`docs/api-reference.md`](docs/api-reference.md)).

## Demo

[**Screen recording of the example app**](video.mov) (47s) — playback, the
imperative commands, marker playback, the dynamic-property overrides, and live
`onMetrics` output.

Neither GitHub nor npm plays a relative `.mov` inline, so that link downloads
the file. For a quick look without downloading, `example/screenshots/` has
stills from both platforms (`android.png`, `ios.png`), and
[`example/README.md`](example/README.md) documents everything the app
exercises.

## Architecture at a glance

```
TypeScript API (src/)         RlottieView + imperative ref. Never touches
                               UIManager or findNodeHandle directly.
        │
Legacy-bridge adapters         iOS: RCTViewManager + Objective-C++ (ios/)
(ios/, android/)                Android: SimpleViewManager + JNI (android/)
        │
Shared C++ core (cpp/)        RlottiePlayerCore over vendored rlottie: parsing,
                               timing, rendering, buffers, caching, cleanup.
```

**Target: React Native Legacy Architecture only** (`newArchEnabled=false`).
Fabric/TurboModules are out of scope for v1 — see the compatibility matrix
below before adding this to a New Architecture app.

## Installation

```sh
npm install react-native-rlottie
# or
yarn add react-native-rlottie
```

### iOS

```sh
cd ios && pod install
```

The podspec declares `s.platforms = { :ios => "13.0" }` — iOS 13.0 is the
minimum deployment target this library builds against
(`react-native-rlottie.podspec`). Autolinking discovers the podspec via
`react-native.config.js`; no manual `Podfile` edit is required for a normal
autolinked install.

### Android

Autolinking picks up `android/build.gradle` automatically. Two things that
build file actually declares and that your app's `android/build.gradle` values
can override via `rootProject.ext` (`safeExtGet`, see the file for the exact
keys):

- `minSdkVersion` defaults to **21**, `compileSdkVersion`/`targetSdkVersion`
  default to **34**.
- The native side is built with CMake + the NDK (default `ndkVersion
"25.1.8937393"`) and ships prebuilt for `arm64-v8a` + `x86_64` by default.
  `armeabi-v7a` is opt-in via `rlottieAbiFilters` and does link, but loses NEON
  acceleration (see [`docs/troubleshooting.md`](docs/troubleshooting.md)).
- This module is Kotlin; the Kotlin Gradle plugin is applied for you.

No manual step beyond a normal `npx react-native run-android` / rebuild is
required.

### Rebuild, don't just reload

This package ships native code. After installing or upgrading it, **rebuild
the app** (`pod install` + a full Xcode/Gradle build) — a Metro/Fast Refresh
reload is not enough, since the native module and view manager themselves have
changed.

## Minimal usage

```tsx
import React, {useRef} from 'react';
import {View} from 'react-native';
import {RlottieView, type RlottieViewRef} from 'react-native-rlottie';

function MyAnimation() {
  const ref = useRef<RlottieViewRef>(null);

  return (
    <View style={{width: 200, height: 200}}>
      <RlottieView
        ref={ref}
        source={{uri: 'asset:///animations/hello.json'}}
        autoPlay
        loop
        onAnimationLoaded={() => ref.current?.play()}
        onAnimationError={e =>
          console.warn(e.nativeEvent.code, e.nativeEvent.message)
        }
      />
    </View>
  );
}
```

`asset:///…` resolves against the platform's bundled-asset root on **both**
iOS and Android — see [`docs/api-reference.md`](docs/api-reference.md) for
every accepted `source` form (`{json}`, `{uri}`, `{path}`), their limits, and
the imperative ref API.

## Supported React Native versions

This library targets **Legacy Architecture only** (`newArchEnabled=false`).
There is no Fabric component and no TurboModule spec in v1 — the layering is
deliberately designed so a future Fabric adapter can reuse the shared C++ core,
but that adapter does not exist yet. **If your app has
`newArchEnabled=true`, this library will not work as a Fabric component; you
must build with the Legacy Architecture.**

| RN version                           | Status                                                                                               |
| ------------------------------------ | ---------------------------------------------------------------------------------------------------- |
| 0.81.0                               | **Verified** — the only version actually built and exercised in this repo's own devDependency.       |
| 0.71.0 – 0.80.x                      | Expected to work (Legacy Architecture), not independently verified.                                  |
| 0.68.0 – 0.70.x                      | Expected to work, not independently verified. See the RN 0.71 note below.                            |
| ≥ 0.82.0 or any New Architecture app | Not supported. `peerDependencies` currently allows `<0.82.0`; this is a ceiling, not a tested claim. |

`peerDependencies.react-native` is `>=0.68.0 <0.82.0` — treat everything
outside RN 0.81.0 itself as "should work per the code's stated assumptions,"
not as covered by CI or device testing in this repo.

Notes worth knowing before you pick a version:

- **The RN 0.71 Maven-artifact boundary matters on Android.**
  `android/build.gradle` resolves the React Native Maven coordinate from the
  installed `react-native` package version: `com.facebook.react:react-android`
  for RN ≥ 0.71, `com.facebook.react:react-native:+` below that. This is
  handled for you automatically; it's listed here so an unusual install layout
  (pnpm, custom hoisting) that defeats the package.json lookup is easy to
  recognize — see `reactNativeVersion` in `android/build.gradle` for the
  override.
- **React version.** `react` is a `peerDependency` here (`"*"`), so your app's
  own React version is what matters at runtime — this library imposes no React
  version of its own. For development, `devDependencies` pin
  `react`/`react-test-renderer` at a matching **19.1.0**, which is what
  `react-native@0.81` itself expects (`peerDependencies.react: ^19.1.0`).

## Further reading

- [`docs/api-reference.md`](docs/api-reference.md) — every prop, event, ref
  method, module function, error code, and source form.
- [`docs/troubleshooting.md`](docs/troubleshooting.md) — diagnosing blank
  views, rejected sources, build/link failures, and reading `onMetrics`.
- [`docs/bridge-contract.md`](docs/bridge-contract.md) — the frozen prop/
  event/command contract iOS and Android must match byte-for-byte.
- [`CHANGELOG.md`](CHANGELOG.md) — release notes.
- [`docs/render-benchmark.md`](docs/render-benchmark.md) — the sync-vs-async
  render measurement behind the "no per-frame allocation" design (development-
  Mac numbers, not device-validated).

## License and third-party notices

MIT-licensed (see the `license` field in `package.json`). This package vendors
[rlottie](https://github.com/Samsung/rlottie) under its own license — see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and `licenses/` for the
full text and the exact pinned revision.
