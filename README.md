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
stills from both platforms and both architectures, and
[`example/README.md`](example/README.md) documents everything the app
exercises. The example app is also the dual-architecture verification harness —
it detects and displays which architecture it is running under, and instruments
the two behaviours most likely to regress under Fabric (see the verification
matrix below).

## Architecture at a glance

```
TypeScript API (src/)         RlottieView + imperative ref. Never touches
                               UIManager or findNodeHandle directly.
        │
Bridge adapters                Legacy: RCTViewManager (ios/) + SimpleViewManager
(ios/, android/)                        + JNI (android/)
                               Fabric: RCTViewComponentView (ios/fabric/) +
                                        the same Android ViewManager, codegen'd
        │
Shared C++ core (cpp/)        RlottiePlayerCore over vendored rlottie: parsing,
                               timing, rendering, buffers, caching, cleanup.
                               Identical on both architectures — untouched by
                               the Fabric work.
```

**Both architectures are supported from one package.** The correct component
and module are selected automatically; see the compatibility matrix below for
the RN >= 0.76 floor that applies to the New Architecture path.

## Installation

```sh
npm install @yevheniionipko/react-native-rlottie
# or
yarn add @yevheniionipko/react-native-rlottie
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

On the New Architecture this matters more than usual: your app's autolinking
output is what registers this library's Fabric component and TurboModule, and a
stale copy of it produces an app that renders through RN's interop fallback
while the real Fabric component is never registered. If the component seems to
work but `Rlottie.isAvailable()` returns `false`, see
[`docs/troubleshooting.md`](docs/troubleshooting.md).

## Minimal usage

```tsx
import React, {useRef} from 'react';
import {View} from 'react-native';
import {
  RlottieView,
  type RlottieViewRef,
} from '@yevheniionipko/react-native-rlottie';

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

This library supports **both architectures from one package**. The Legacy
Architecture path (`newArchEnabled=false`) is unchanged and remains supported
down to RN 0.68. A real Fabric component and a real TurboModule are also
shipped, and are selected automatically when your app runs the New
Architecture — there is nothing to configure and no separate entry point.

**New Architecture support requires RN >= 0.76.** Below that, iOS third-party
component registration used a different generated provider and bridgeless was
not yet the default, so supporting it would mean shipping two registration
mechanisms. On RN < 0.76 with `newArchEnabled=true` the component will fail to
resolve rather than silently misbehave. The Legacy path still covers 0.68+.

| RN version      | Legacy Architecture                                   | New Architecture                              |
| --------------- | ----------------------------------------------------- | --------------------------------------------- |
| >= 0.82.0       | Removed by React Native itself.                       | Expected to work, not independently verified. |
| 0.81.0          | **Verified** — see the verification matrix below.     | **Verified** — see the matrix below.          |
| 0.76.0 – 0.80.x | Expected to work, not independently verified.         | Expected to work, not independently verified. |
| 0.71.0 – 0.75.x | Expected to work, not independently verified.         | **Not supported** (see the 0.76 floor above). |
| 0.68.0 – 0.70.x | Expected to work, not verified. See the RN 0.71 note. | **Not supported.**                            |

`peerDependencies.react-native` is `>=0.68.0` — open-ended. It previously
carried a `<0.82.0` ceiling, which existed only because this library was
Legacy-only and RN 0.82 drops the Legacy Architecture. Now that a real Fabric
component and TurboModule ship, that ceiling was blocking installs for no
reason and has been removed. Treat everything outside RN 0.81.0 itself as
"should work per the code's stated assumptions," not as covered by CI or
device testing in this repo.

On **RN >= 0.82 the New Architecture is the only option**, so this library's
Fabric path is the one that runs there — nothing to configure. The JS layer is
already safe on such a runtime: `UIManager` and `findNodeHandle` are
dereferenced only inside the Legacy dispatch branch, which is never taken when
Fabric is active. What has **not** been checked on 0.82+ is whether the Legacy
_native_ adapter sources still compile against an RN that removed the old
architecture; they are still shipped in the package and built unconditionally.
If you hit that, please open an issue with the compiler output.

### What "verified" actually means

Honest scope, because "supports the New Architecture" is easy to overclaim:

| configuration     | evidence                                                                                                   |
| ----------------- | ---------------------------------------------------------------------------------------------------------- |
| Android, Legacy   | **Runtime** — example app run on an API 37 emulator.                                                       |
| Android, New Arch | **Runtime** — same, with the Fabric registration confirmed in the generated `autolinking.cpp`.             |
| iOS, Legacy       | **Build only** — `pod install` + `xcodebuild` succeed; app not launched.                                   |
| iOS, New Arch     | **Build only** — same, with the Fabric component view confirmed compiled under `-DRCT_NEW_ARCH_ENABLED=1`. |

On Android under Fabric, both behaviours most at risk from the architecture
switch were measured rather than assumed: an unrelated style-only re-render
does **not** restart playback or re-resolve the source (26 re-renders → 0 of
either), and no `onAnimationLoop` events are lost to event coalescing (loop
counts match wall-clock expectation exactly over runs up to 98 s).

The iOS rows are deliberately not upgraded. Both configurations compile and
link, but the app was not launched on a Simulator, and — as the Android work
proved — **a successful build is not evidence that the Fabric component is
registered**: a stale autolinking cache produced an app that rendered
correctly through RN's ViewManager interop fallback while our own
`ComponentDescriptor` was never registered at all. See
[`docs/troubleshooting.md`](docs/troubleshooting.md) if you suspect this.

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
- [`docs/new-architecture-design.md`](docs/new-architecture-design.md) — how
  Fabric/TurboModule support is layered alongside the Legacy path without
  touching the shared C++ core, and the verification behind each claim.
- [`CHANGELOG.md`](CHANGELOG.md) — release notes.
- [`docs/render-benchmark.md`](docs/render-benchmark.md) — the sync-vs-async
  render measurement behind the "no per-frame allocation" design (development-
  Mac numbers, not device-validated).

## License and third-party notices

MIT-licensed (see the `license` field in `package.json`). This package vendors
[rlottie](https://github.com/Samsung/rlottie) under its own license — see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and `licenses/` for the
full text and the exact pinned revision.
