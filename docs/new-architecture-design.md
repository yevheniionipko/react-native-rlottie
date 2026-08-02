# New Architecture (Fabric + TurboModules) design

Status: **design, not implemented.** This document is the reference the
implementing chunks are written against. Where it says "must", a deviation is a
bug; where it says "verify", the fact was not provable from source alone and the
implementing chunk owns proving it.

Everything here was checked against the vendored RN in `node_modules/react-native`
at **0.81.0** and `node_modules/@react-native/codegen` at **0.81.0**. File paths
with line numbers point at that vendored copy or at this repo. Where a claim
comes from reading RN's own source, the path is given so a future reader can
re-verify it after an RN bump instead of trusting this file.

## The decision, restated

Dual-architecture support in one package:

- The Legacy Architecture path (`RCTViewManager` + Obj-C++ on iOS,
  `SimpleViewManager` + JNI on Android) keeps working for `newArchEnabled=false`
  consumers. It is **not** deleted, forked, or reimplemented.
- A real Fabric component and a real TurboModule are **added** alongside it,
  both calling the same `cpp/` core. `RlottiePlayerCore`, `RenderCoordinator`,
  `FrameSink`, `FrameBuffer`, `PlaybackController`, `Metrics`,
  `ModelCacheController`, `InputLimits`, `CacheKey` are **not touched by this
  work at all**. If a chunk finds itself editing `cpp/`, the design is wrong and
  it should stop and re-read this document.
- RN's `LegacyViewManagerInteropComponentDescriptor` was investigated and
  rejected upstream of this document. Not revisited here.

The reason the core needs no changes is worth stating precisely, because it is
the load-bearing claim of the whole design: **every Fabric callback that would
touch the core runs on the platform UI thread**, which is exactly the thread the
Legacy adapters already call from. See "Threading, settled" below.

## Minimum React Native version for the new-architecture path

**New-architecture support requires RN >= 0.76.** The Legacy path keeps its
existing `>=0.68.0 <0.82.0` range; `package.json`'s `peerDependencies` does not
change. The floor is documented, enforced by nothing, and stated in the README:
a 0.74 app with `newArchEnabled=true` will fail to find the component rather
than silently misbehave.

Why 0.76 and not lower:

- iOS third-party component registration went through two names. 0.81 generates
  `RCTThirdPartyComponentsProvider.{h,mm}`
  (`node_modules/react-native/scripts/codegen/generate-artifacts-executor/generateRCTThirdPartyComponents.js:43`);
  older releases generated `RCTThirdPartyFabricComponentsProvider`. Supporting
  both means shipping two registration mechanisms for no benefit.
- 0.76 is where bridgeless is the default for the new architecture, so
  "new arch" and "bridgeless" collapse into one configuration to test instead of
  four.
- `BaseReactPackage`
  (`node_modules/react-native/ReactAndroid/src/main/java/com/facebook/react/BaseReactPackage.kt:23`)
  and `UIManagerHelper.getEventDispatcherForReactTag`
  (`.../uimanager/UIManagerHelper.kt:107`) are both stable at 0.76 and work on
  both architectures, which is what lets Android use one code path.

Everything below is written against 0.81 and is expected to hold on 0.76–0.81.
The first implementation chunk owns confirming that on the lowest supported
version, because codegen output details do drift.

---

# 1. Package layout and dual-architecture codegen

## 1.1 Where the specs live

```
src/specs/RlottieViewNativeComponent.ts    # Fabric component spec + commands
src/specs/NativeRlottieModule.ts           # TurboModule spec
```

Both filenames are **load-bearing, not convention**:

- `@react-native/babel-plugin-codegen` only rewrites a `codegenNativeComponent`
  call in a file whose name matches `/NativeComponent\.(js|ts)$/`
  (`node_modules/@react-native/babel-plugin-codegen/index.js:57`). A file named
  `RlottieViewSpec.ts` is silently left alone, `codegenNativeComponent` falls
  through to its runtime body, and under bridgeless you get a dev warning plus a
  `requireNativeComponent` call that cannot work
  (`node_modules/react-native/Libraries/Utilities/codegenNativeComponent.js:37-42`).
  Note the regex accepts `.ts` but **not `.tsx`** — the spec must not be a
  `.tsx` file.
- The TurboModule spec must be `Native<Name>.ts` for the module-side codegen to
  pick it up, and the exported interface must be named `Spec`
  (`node_modules/@react-native/codegen/lib/parsers/typescript/parser.js:122-128`
  rejects anything else).

Type imports come from the namespace form, which the TS parser resolves through
`TSQualifiedName`
(`node_modules/@react-native/codegen/lib/parsers/typescript/parser.js:98-116`):

```ts
import type {CodegenTypes, HostComponent, ViewProps} from 'react-native';
```

`react-native/types/index.d.ts:141` re-exports the codegen types as the
`CodegenTypes` namespace; the older deep path
`react-native/Libraries/Types/CodegenTypes` has no `.d.ts` in 0.81 and will not
typecheck.

## 1.2 `codegenConfig` in `package.json`

```json
"codegenConfig": {
  "name": "RNRlottieSpec",
  "type": "all",
  "jsSrcsDir": "src/specs",
  "android": {
    "javaPackageName": "com.rlottie.spec"
  },
  "ios": {
    "componentProvider": {
      "RlottieView": "RNRlottieComponentView"
    },
    "components": {
      "RlottieView": {"className": "RNRlottieComponentView"}
    },
    "modules": {
      "RlottieModule": {"className": "RNRlottieModule"}
    }
  }
}
```

Decisions and traps in that block:

- **`name: "RNRlottieSpec"`** becomes the autolinking `libraryName`
  (`example/node_modules/@react-native-community/cli-config-android/build/config/findLibraryName.js:22-33`
  reads `codegenConfig.name` first). It therefore also becomes the Android CMake
  target name `react_codegen_RNRlottieSpec` and the C++ include prefix
  `react/renderer/components/RNRlottieSpec/`. It is a public-ish identifier:
  changing it later breaks any app that pinned `libraryName` in its own
  `react-native.config.js`.
- **`type: "all"` is mandatory, not cosmetic.** `type: "components"` makes the
  module-provider generator skip us entirely
  (`generateRCTModuleProviders.js:53-63`), and on Android it means codegen
  produces no `jni/CMakeLists.txt` — while the app's autolinking
  unconditionally `add_subdirectory`s
  `<lib>/android/build/generated/source/codegen/jni/CMakeLists.txt`
  (`node_modules/@react-native/gradle-plugin/react-native-gradle-plugin/src/main/kotlin/com/facebook/react/tasks/GenerateAutolinkingNewArchitecturesFileTask.kt:56-70`).
  A missing file there is a CMake configure failure in the consuming app, not a
  graceful skip.
- **`jsSrcsDir: "src/specs"`** scopes three separate things: the Gradle schema
  task's input tree (`ReactPlugin.kt:158-166`), the iOS codegen script's input,
  and the CLI's `findComponentDescriptors` glob
  (`findComponentDescriptors.js:37-47`). Pointing it at `src` instead would make
  all three walk the whole TS surface on every build for no reason.
- **`android.javaPackageName` only affects TurboModule specs.** The generated
  view-manager interface and delegate are emitted into the hardcoded package
  `com.facebook.react.viewmanagers` regardless of this setting. Do not spend
  time trying to move them.
- **Both `ios.componentProvider` and `ios.components` are declared, deliberately.**
  0.81 branches: a library that sets `ios.components`, or sets neither, goes
  down the newer annotation path; a library that sets only `componentProvider`
  goes down the older path
  (`generateRCTThirdPartyComponents.js:65-72`). `ios.components` does not exist
  before 0.81, so declaring only it would break 0.76–0.80; declaring only
  `componentProvider` works everywhere including 0.81. Declaring both is the
  portable choice: newer RN uses the newer key, older RN uses the older one, and
  neither reads the other. **They must name the same class** — a mismatch is a
  registration that silently resolves on one RN version and not another.
- **If neither key is set, 0.81 crawls every `.mm` in the package looking for
  `RCTComponentViewProtocol` conformance** and logs a deprecation
  (`generateRCTThirdPartyComponents.js:115-133`). Our `ios/` tree is large and
  that crawl is both slow and fragile. Do not rely on it.
- The generated provider builds an `NSDictionary` literal containing
  `NSClassFromString(@"RNRlottieComponentView")`
  (`generateRCTThirdPartyComponents.js:139`). If the class is not linked into
  the app — for example because the Fabric source file was excluded from the
  build — `NSClassFromString` returns nil and the dictionary literal **throws at
  load time**. A typo in the class name is an app that will not start, not a
  missing component. Verifying the exact class name is part of the iOS chunk's
  acceptance criteria.

`package.json`'s `files` array already includes `src`, so `src/specs` ships
without change. `scripts/verify-npm-package.sh` should gain an assertion that
both spec files are present in the packed tarball — a published package missing
its specs produces a codegen no-op in the consumer, which surfaces as
"component not found" far from the cause.

## 1.3 `react-native.config.js`

The CLI derives `libraryName`, `componentDescriptors`, and `cmakeListsPath`
automatically (`cli-config-android/build/config/index.js:133-138`), so nothing
is strictly required. Pin them anyway:

```js
module.exports = {
  dependency: {
    platforms: {
      ios: {},
      android: {
        libraryName: 'RNRlottieSpec',
        componentDescriptors: ['RlottieViewComponentDescriptor'],
        cmakeListsPath: 'build/generated/source/codegen/jni/CMakeLists.txt',
      },
    },
  },
};
```

Rationale: `findComponentDescriptors` works by globbing the package's JS/TS and
regex-scanning for `codegenNativeComponent`
(`findComponentDescriptors.js:43-52`). That is a heuristic over source text. If
it misses, the app's generated `autolinking.cpp` omits our
`ComponentDescriptor` registration and the component renders as an unimplemented
view — a silent, hard-to-attribute failure. Pinning the value makes the
contract explicit and removes the heuristic from the failure surface. The cost is
one more place to update if the component is ever renamed; that is a fair trade
against a silent mis-registration.

## 1.4 iOS build integration (podspec)

The current podspec (`react-native-rlottie.podspec`) has two subspecs,
`rlottie` and `core`, and depends only on `React-Core`. Changes:

```ruby
  # ... existing subspecs unchanged ...
  s.default_subspecs = "core"

  # MUST be the last statement in the spec block. See below.
  install_modules_dependencies(s)
```

with, at the top of the file:

```ruby
require File.join(File.dirname(`node --print "require.resolve('react-native/package.json')"`), "scripts/react_native_pods")
```

Facts behind that shape:

- `install_modules_dependencies(spec)`
  (`node_modules/react-native/scripts/react_native_pods.rb:273`, implemented at
  `scripts/cocoapods/new_architecture.rb:73-138`) adds every Fabric/TurboModule
  pod dependency **unconditionally** and makes only the `-DRCT_NEW_ARCH_ENABLED=1`
  compiler flag conditional (`new_architecture.rb:46-49, 101, 111`). Those pods
  always exist: `use_react_native!` calls `setup_fabric!` and installs
  `ReactCodegen` regardless of `new_arch_enabled`
  (`react_native_pods.rb:177-196`). So calling it in an old-arch build is safe;
  it costs pod graph size, not correctness.
- **It must be called last** because it *reads then overwrites*
  `spec.compiler_flags` and `spec.pod_target_xcconfig`
  (`new_architecture.rb:76-104`). Anything we set after it is lost; anything we
  set before it is preserved and appended to.
- It sets `CLANG_CXX_LANGUAGE_STANDARD` to RN's own standard, which in 0.81 is
  **C++20**. Our `core` subspec currently pins `c++17`. That pin becomes
  ineffective, and the shared core will compile as C++20. This is believed
  harmless (the core is plain C++17 with no removed-in-C++20 constructs) but it
  is a real change to how shipped code is compiled and is called out as a
  verification item, not an assumption.
- The vendored rlottie subspec's `-std=gnu++14 -fno-exceptions -fno-rtti
  -U__ARM_NEON__` are **per-file `compiler_flags`**, which appear after the
  target-level xcconfig standard on the command line and therefore still win.
  This is why rlottie survives the C++20 switch. It is also why those flags must
  stay in `compiler_flags` and must not be "tidied" into `pod_target_xcconfig`:
  all subspecs of one pod share a single pod target and a single merged
  xcconfig, so an xcconfig-level `-std=gnu++14` would apply to our C++20 core
  too.

**Fabric sources are added to `source_files` unconditionally and guarded
internally by `#ifdef RCT_NEW_ARCH_ENABLED`.** The alternative — a conditional
`source_files` list keyed off `ENV['RCT_NEW_ARCH_ENABLED']` — depends on that
variable being set before our podspec is evaluated. It is: `use_react_native!`
sets it (`react_native_pods.rb:101`) and CocoaPods evaluates the entire Podfile
before reading podspecs during resolution. But the `#ifdef` form does not
*depend* on that ordering being true, and it keeps one file list for both
architectures, so it is the one to use. The whole body of each new Fabric file
sits inside the guard, so in an old-arch build the translation unit compiles to
nothing.

## 1.5 Android build integration (`android/build.gradle`)

Add `apply plugin: "com.facebook.react"` with the plugin on the buildscript
classpath. What that buys, verified in
`node_modules/@react-native/gradle-plugin/react-native-gradle-plugin/src/main/kotlin/com/facebook/react/ReactPlugin.kt`:

- Registers `generateCodegenSchemaFromJavaScript` and
  `generateCodegenArtifactsFromSchema`, wired into `preBuild`
  (`ReactPlugin.kt:233`).
- For a library, the tasks run **unconditionally** — the `onlyIf` is
  `isLibrary || needsCodegenFromPackageJson` (`ReactPlugin.kt:213`). Codegen
  therefore runs even with `newArchEnabled=false`, which is exactly what we
  want: the generated Java interfaces are always on the compile classpath, so
  the ViewManager and the module can implement them with **no `src/newarch` vs
  `src/oldarch` source-set split**. This is the single biggest simplification on
  Android and it is worth protecting: any change that makes generated Java
  conditionally available reintroduces the split.
- Adds `build/generated/source/codegen/java` to the `main` source set
  (`ReactPlugin.kt:219-224`).

Two Android build risks:

1. **The standalone library build breaks.** `scripts/check-android-build.sh`
   builds this module with nothing inherited from an app. The `com.facebook.react`
   plugin needs `reactNativeDir` and a node executable to run codegen. Mitigation:
   the script must set `react { reactNativeDir = file("../node_modules/react-native") }`
   (or the plugin's expected root) and the buildscript must add
   `classpath("com.facebook.react:react-native-gradle-plugin")` resolved from
   `node_modules`. The chunk that adds the plugin owns keeping this script green;
   a standalone build that silently stops producing an AAR is the failure mode
   Chunk 3.5 already got bitten by once.
2. **`android/src/main/cpp/react-native-rlottie.expmap` is untouched, and that
   is correct.** The C++ `ComponentDescriptor` and the TurboModule provider live
   in a **separate** shared library, `libreact_codegen_RNRlottieSpec.so`, built
   from our generated `jni/` directory by the *app's* CMake
   (`GenerateAutolinkingNewArchitecturesFileTask.kt:56-88`), and registered by
   the app's generated `autolinking.cpp`. Our own
   `libreact-native-rlottie.so` continues to export only
   `Java_com_rlottie_*` and `JNI_OnLoad`. Worth stating explicitly because if
   anyone ever tries to hand-write a components registry inside our `.so`, that
   version script will hide its entry points and produce exactly the
   "loads fine, fails at first call" failure the Chunk 3.5 notes describe.

No new C++ is compiled into our `.so` for the new architecture, on either
platform.

---

# 2. TurboModule: `RlottieModule`

## 2.1 Current shape

`ios/RNRlottieModule.mm` exports `RlottieModule` with three
`RCT_REMAP_METHOD`s, all Promise-based, all incapable of failing
(`(void)reject` in each). `android/src/main/java/com/rlottie/RlottieModule.kt`
exports the same three as Promise-based `@ReactMethod`s, and *can* reject
(`INVALID_ARGUMENT` for a negative `modelCacheSize`, plus a catch-all per
method). `src/RlottieModule.ts` adds a JS-only fourth entry point,
`isAvailable()`, which is `NativeModules.RlottieModule != null`.

The Promise-on-both-platforms rule is frozen in
`docs/bridge-contract.md` ("Global module") and does not change.

## 2.2 The spec

`src/specs/NativeRlottieModule.ts`:

```ts
import type {CodegenTypes, TurboModule} from 'react-native';
import {TurboModuleRegistry} from 'react-native';

export interface Spec extends TurboModule {
  // `options` is UnsafeObject on purpose — see below.
  configure(options: CodegenTypes.UnsafeObject): Promise<void>;
  clearModelCache(): Promise<void>;
  getNativeVersion(): Promise<{rlottieCommit: string; modelCacheSize: number}>;
}

export default TurboModuleRegistry.get<Spec>('RlottieModule');
```

Three deliberate choices:

- **`configure`'s argument is `UnsafeObject`, not a typed object.** A typed
  object argument makes codegen emit a C++/Obj-C struct
  (`JS::NativeRlottieModule::SpecConfigureOptions`) into the generated protocol.
  That protocol is what the *same* Obj-C class must satisfy, and a struct
  parameter cannot be expressed as an old-architecture `RCT_EXPORT_METHOD` — so
  a typed argument forces the module implementation itself to be
  `#ifdef`-split into two method bodies. `UnsafeObject` generates `NSDictionary *`
  on iOS and `ReadableMap` on Android, which is exactly what both existing
  implementations already take. One class, one body, both architectures. The
  cost is no codegen validation of `{modelCacheSize}` — acceptable: it is one
  optional number, both platforms already validate it, and Android already
  rejects a negative value with `INVALID_ARGUMENT`.
- **`getNativeVersion`'s return type is typed.** A `Promise` resolution goes
  through `resolve()`, which takes an untyped `id`/`Object` on both platforms
  regardless of the declared type, so typing it constrains nothing native while
  giving the JS side and the generated Flow/TS types something real.
- **`TurboModuleRegistry.get`, not `getEnforcing`.** `getEnforcing` throws at
  spec-module import time when the native module is absent. `src/RlottieModule.ts`
  exists in its current lazy form specifically so that importing `RlottieView`
  in a Jest test, a storybook, or a web build does not explode (see its header
  comment). `get` returns null and preserves that, and keeps `isAvailable()`
  meaningful.

## 2.3 iOS implementation

`ios/RNRlottieModule.mm` is **modified in place**, not replaced:

- Keep `RCT_EXPORT_MODULE(RlottieModule)` and
  `+ (BOOL)requiresMainQueueSetup { return NO; }`. Both are still honoured
  under TurboModules, and `requiresMainQueueSetup` returning NO is why we do
  *not* list the module in any main-queue-setup provider.
- Declare conformance to the generated protocol and add the TurboModule factory,
  both behind the guard:

```objc
#ifdef RCT_NEW_ARCH_ENABLED
#import <RNRlottieSpec/RNRlottieSpec.h>
@interface RNRlottieModule : NSObject <NativeRlottieModuleSpec>
@end
#else
@interface RNRlottieModule : NSObject <RCTBridgeModule>
@end
#endif
```

and inside the implementation:

```objc
#ifdef RCT_NEW_ARCH_ENABLED
- (std::shared_ptr<facebook::react::TurboModule>)getTurboModule:
    (const facebook::react::ObjCTurboModule::InitParams &)params
{
  return std::make_shared<facebook::react::NativeRlottieModuleSpecJSI>(params);
}
#endif
```

- **The three method selectors must match the generated protocol exactly.** The
  current file uses `RCT_REMAP_METHOD`, which invents its own selector
  (`configureWithOptions:resolver:rejecter:`). The generated protocol will
  declare `- (void)configure:(NSDictionary *)options
  resolve:(RCTPromiseResolveBlock)resolve reject:(RCTPromiseRejectBlock)reject;`.
  So the chunk must switch to plain `RCT_EXPORT_METHOD` with the codegen
  selector names and verify against the actual generated header rather than
  against this document. A near-miss here compiles with a protocol warning and
  fails at runtime with "method not found" only under the new architecture.
- Method **bodies do not change**: same forwards into
  `rnrlottie::ModelCacheController` and `rnrlottie::kRlottieCommit`.

Threading: TurboModule methods for a module with `requiresMainQueueSetup == NO`
run on the JS thread or the module's method queue, not main.
`ModelCacheController` is process-global and internally serialized
(`cpp/ModelCacheController.h`), so this is unchanged from today and needs no new
locking.

## 2.4 Android implementation

`RlottieModule.kt` extends the generated abstract spec instead of
`ReactContextBaseJavaModule`:

```kotlin
class RlottieModule(reactContext: ReactApplicationContext) :
    NativeRlottieModuleSpec(reactContext) {
  override fun configure(options: ReadableMap?, promise: Promise) { /* unchanged body */ }
  override fun clearModelCache(promise: Promise) { /* unchanged */ }
  override fun getNativeVersion(promise: Promise) { /* unchanged */ }
}
```

`NativeRlottieModuleSpec` is generated into `com.rlottie.spec` (from
`codegenConfig.android.javaPackageName`), itself extends
`ReactContextBaseJavaModule`, and implements `TurboModule`. Because library
codegen runs on both architectures (§1.5), this compiles with
`newArchEnabled=false` too — one class, no source-set split. `getName()` becomes
`final` in the generated spec with a `NAME` constant, so the current
`override fun getName()` and the private `NAME` constant are removed.

`RlottiePackage.kt` becomes a `BaseReactPackage`:

```kotlin
class RlottiePackage : BaseReactPackage() {
  override fun getModule(name: String, ctx: ReactApplicationContext): NativeModule? =
      if (name == RlottieModule.NAME) RlottieModule(ctx) else null

  override fun getReactModuleInfoProvider(): ReactModuleInfoProvider = /* one entry */

  override fun createViewManagers(ctx: ReactApplicationContext) =
      mutableListOf<ViewManager<*, *>>(RlottieViewManager())
}
```

`BaseReactPackage` is a `ReactPackage` and works on both architectures
(`BaseReactPackage.kt:23`); it throws only if someone calls the deprecated
`createNativeModules` (`BaseReactPackage.kt:26-29`), which nothing on either
architecture does once `getModule` is implemented. `ReactModuleInfo` needs
`isTurboModule = true` and `canOverrideExistingModule = false`;
`isCxxModule = false`.

## 2.5 `src/RlottieModule.ts`

Resolution changes from `NativeModules[MODULE_NAME]` to importing the spec's
default export (which is `TurboModuleRegistry.get(...)` — and on the old
architecture `TurboModuleRegistry.get` falls back to `NativeModules`, so this is
one code path). The lazy `requireNative()` wrapper, the `LINKING_ERROR` text,
and `isAvailable()` all stay as they are. `Rlottie.configure/clearModelCache/
getNativeVersion` signatures do not change, so `src/index.ts`,
`docs/api-reference.md`, and every consumer are unaffected.

---

# 3. Fabric component: `RlottieView`

## 3.1 The spec file

`src/specs/RlottieViewNativeComponent.ts`, sketched (types abbreviated; the real
file spells them out):

```ts
import type {CodegenTypes, HostComponent, ViewProps} from 'react-native';
import codegenNativeCommands from 'react-native/Libraries/Utilities/codegenNativeCommands';
import codegenNativeComponent from 'react-native/Libraries/Utilities/codegenNativeComponent';

type SourceProp = Readonly<{
  json?: string;
  uri?: string;
  path?: string;
  cacheKey?: string;
  resourcePath?: string;
}>;

type ColorOverride = Readonly<{keyPath: string; color: string}>;
type OpacityOverride = Readonly<{keyPath: string; opacity: CodegenTypes.Double}>;
type StrokeWidthOverride = Readonly<{keyPath: string; width: CodegenTypes.Double}>;

type LoadedEvent = Readonly<{
  width: CodegenTypes.Int32;
  height: CodegenTypes.Int32;
  duration: CodegenTypes.Double;
  frameRate: CodegenTypes.Double;
  totalFrames: CodegenTypes.Int32;
  markers: ReadonlyArray<
    Readonly<{
      name: string;
      startFrame: CodegenTypes.Int32;
      endFrame: CodegenTypes.Int32;
    }>
  >;
}>;

type ErrorEvent = Readonly<{code: string; message: string}>;

type MetricsEvent = Readonly<{
  parseMs: CodegenTypes.Double;
  firstFrameMs: CodegenTypes.Double;
  renderP50Ms: CodegenTypes.Double;
  renderP95Ms: CodegenTypes.Double;
  renderP99Ms: CodegenTypes.Double;
  framesRendered: CodegenTypes.Double;
  framesDropped: CodegenTypes.Double;
  bufferAllocCount: CodegenTypes.Double;
  peakBufferBytes: CodegenTypes.Double;
  uiStallCount: CodegenTypes.Double;
  uiStallMaxMs: CodegenTypes.Double;
}>;

interface NativeProps extends ViewProps {
  source?: SourceProp;

  autoPlay?: CodegenTypes.WithDefault<boolean, false>;
  loop?: CodegenTypes.WithDefault<boolean, false>;
  repeatCount?: CodegenTypes.WithDefault<CodegenTypes.Int32, 0>;
  speed?: CodegenTypes.WithDefault<CodegenTypes.Double, 1.0>;
  progress?: CodegenTypes.WithDefault<CodegenTypes.Double, 0.0>;
  startFrame?: CodegenTypes.WithDefault<CodegenTypes.Int32, 0>;
  endFrame?: CodegenTypes.WithDefault<CodegenTypes.Int32, 0>;
  // string, NOT a string-literal union — see 3.2.3.
  resizeMode?: CodegenTypes.WithDefault<string, 'contain'>;
  renderScale?: CodegenTypes.WithDefault<CodegenTypes.Double, 1.0>;
  pauseWhenInactive?: CodegenTypes.WithDefault<boolean, true>;
  cacheStrategy?: CodegenTypes.WithDefault<string, 'model'>;
  colorOverrides?: ReadonlyArray<ColorOverride>;
  opacityOverrides?: ReadonlyArray<OpacityOverride>;
  strokeWidthOverrides?: ReadonlyArray<StrokeWidthOverride>;
  metricsEnabled?: CodegenTypes.WithDefault<boolean, false>;

  onAnimationLoaded?: CodegenTypes.DirectEventHandler<LoadedEvent>;
  onAnimationError?: CodegenTypes.DirectEventHandler<ErrorEvent>;
  onAnimationStart?: CodegenTypes.DirectEventHandler<null>;
  onAnimationPause?: CodegenTypes.DirectEventHandler<null>;
  onAnimationLoop?: CodegenTypes.DirectEventHandler<null>;
  onAnimationFinish?: CodegenTypes.DirectEventHandler<null>;
  onMetrics?: CodegenTypes.DirectEventHandler<MetricsEvent>;
}

interface NativeCommands {
  play: (ref: React.ElementRef<HostComponent<NativeProps>>, startFrame: CodegenTypes.Int32, endFrame: CodegenTypes.Int32) => void;
  pause: (ref: React.ElementRef<HostComponent<NativeProps>>) => void;
  resume: (ref: React.ElementRef<HostComponent<NativeProps>>) => void;
  stop: (ref: React.ElementRef<HostComponent<NativeProps>>) => void;
  reset: (ref: React.ElementRef<HostComponent<NativeProps>>) => void;
  seekToProgress: (ref: React.ElementRef<HostComponent<NativeProps>>, progress: CodegenTypes.Double) => void;
  seekToFrame: (ref: React.ElementRef<HostComponent<NativeProps>>, frame: CodegenTypes.Int32) => void;
  setSpeed: (ref: React.ElementRef<HostComponent<NativeProps>>, speed: CodegenTypes.Double) => void;
  playMarker: (ref: React.ElementRef<HostComponent<NativeProps>>, name: string) => void;
}

export const Commands: NativeCommands = codegenNativeCommands<NativeCommands>({
  supportedCommands: [
    'play', 'pause', 'resume', 'stop', 'reset',
    'seekToProgress', 'seekToFrame', 'setSpeed', 'playMarker',
  ],
});

export default codegenNativeComponent<NativeProps>('RlottieView');
```

`allowRemoteSources` is deliberately absent: it is a JS-only gate consumed by
`normalizeSource` and, per `docs/bridge-contract.md`'s "Remote loading policy",
must never cross the bridge on either architecture.

The first parameter of every command **must** be typed
`React.ElementRef<...>` or the parser throws
(`node_modules/@react-native/codegen/lib/parsers/typescript/components/commands.js:20-38`).

## 3.2 Props → generated `Props` struct

Codegen support was verified rather than assumed. `getNativeTypeFromAnnotation`
(`node_modules/@react-native/codegen/lib/generators/components/ComponentsGeneratorUtils.js:20-95`)
maps: object → a generated struct by value; array-of-object → `std::vector<Struct>`;
string-literal union → a generated `enum class`; plain string → `std::string`;
`UnsafeMixed` → `folly::dynamic`. On Android the generated interface takes
`@Nullable ReadableMap` for an object, `@Nullable ReadableArray` for an array,
`String` for both plain strings and enums, and raw primitives otherwise
(`GeneratePropsJavaInterface.js:78-110`, `GeneratePropsJavaDelegate.js:114-125`).

### 3.2.1 `source` — a typed object, compared on `cacheKey`

`source` is modelled as a typed object (`SourceProp` above) rather than
`UnsafeMixed`. Consequences and the reasoning:

- **Android gets `@Nullable ReadableMap`, which is byte-identical to the
  existing `@ReactProp(name = "source") fun setSource(view, source: ReadableMap?)`.**
  `RlottieSourceResolver.resolve(context, source)` and its 500 lines of security
  validation are untouched. `UnsafeMixed` would give `Dynamic` and force a
  signature change plus an `.asMap()` at the top of a security-sensitive path,
  for no gain.
- **iOS gets a `RlottieViewSourceStruct` with five `std::string` fields.** The
  component view rebuilds an `NSDictionary` from it and hands that to the
  existing `RNRlottieSourceResolver`, so that class is also untouched. That
  rebuild is roughly ten lines and is the entire cost of this choice.
- **Nullability is lost, and that is fine.** A generated object prop is a value,
  never `std::optional`, so "source absent" and "source with all-empty fields"
  are the same thing. It does not matter: both resolvers already treat an empty
  string as absent (`ios/RNRlottieView.mm:625`, `:636` test `length > 0`), and
  under Fabric the signal for "do nothing" is no longer "the setter wasn't
  called" but "the value equals `oldProps`". Also note that when a prop is
  missing from the raw props, codegen's `convertRawProp` keeps the *previous*
  value — so a JS `source={undefined}` inherits the last value and compares
  equal, which reproduces exactly the Legacy behaviour that
  `src/RlottieView.tsx` relies on when it drops `source` to `undefined` on a
  normalization failure ("both native resolvers treat a null source as no
  change").

**Equality rule, mandatory on both platforms.** Under Fabric the native side
receives a complete prop set on every commit (see §3.4.4), so `setSource` must
be guarded by a change check or every unrelated style change re-resolves the
source, re-reads the file, re-copies the `content://` URI, and bumps the
generation — restarting the animation. The comparison is:

1. If either side's `cacheKey` is empty → treat as **changed** (conservative;
   matches today's "apply whatever arrives" behaviour for a hand-rolled caller).
2. Otherwise compare `cacheKey`, `path`, `uri`, and `resourcePath` for string
   equality. **Never compare the `json` body.**

Comparing `cacheKey` instead of `json` is exact, not an approximation, because
`cacheKey` is content-addressed: `src/source.ts` produces
`sha256(json):callerCacheKey` and `cpp/CacheKey.h` recomposes it natively over
the real bytes. Equal `cacheKey` implies equal JSON by construction. Comparing a
multi-megabyte `json` string on every commit would be a per-commit O(n) cost on
the UI thread, which is the kind of thing this library exists to avoid.

Also note the generated struct has **no `operator==`** in a normal build —
`StructTemplate` puts it behind `#ifdef RN_SERIALIZABLE_STATE`
(`GeneratePropsH.js:120-127`). So the comparison must be hand-written whatever
we do; do not write `oldProps.source == newProps.source` and expect it to
compile, and do not start relying on `RN_SERIALIZABLE_STATE` either way.

### 3.2.2 The three override arrays

`std::vector<RlottieViewColorOverridesStruct>` etc. on iOS,
`@Nullable ReadableArray` on Android. Same `operator==` problem: `std::vector`'s
`==` needs the element's `==`, which is not generated. Each platform gets a small
hand-written comparator (size, then `keyPath` + value per element). Without it,
every commit re-enqueues N `setColor`/`setOpacity`/`setStrokeWidth` items onto
`RenderCoordinator`'s control queue. That is not a correctness bug — the setters
are idempotent and generation-gated — but it is avoidable per-commit work on the
render worker, and the whole point of routing overrides through the control queue
was to keep that queue meaningful.

Parsing itself is unchanged: the same hex parse, the same finite-number check,
the same "skip a malformed entry rather than fail the prop apply", the same
clamp-natively-don't-reject rule from `docs/bridge-contract.md`'s Phase 6
section.

### 3.2.3 `resizeMode` and `cacheStrategy` are `string`, NOT string-literal unions

This is the single most important prop-modelling decision here.

A string-literal union prop generates an `enum class` plus:

```cpp
static inline void fromRawValue(const PropsParserContext& context,
                                const RawValue& value, EnumName& result) {
  auto string = (std::string)value;
  /* if/else per known value */
  abort();
}
```

— `GeneratePropsH.js:66-74`. **An unrecognized string calls `abort()`.** That is
a hard process kill, it happens while parsing props on the shadow/background
thread, and it is not catchable.

Both Legacy adapters currently do the opposite, deliberately:
`ios/RNRlottieView.mm:496-507` falls back to `contain` for anything
unrecognized, and `RlottieViewManager.kt:265-267` validates against a set and
returns without applying — with the explicit comment "an untrusted/typo'd JS
prop must never crash the whole app from a UI-thread prop apply". TypeScript
prevents a bad value for TS consumers, but the props that arrive natively come
from untyped JS; a JS-only consumer, an `any`, a spread of server-driven props,
or a stale bundle against a newer native binary all produce a string the enum
does not know.

So: declare both as `WithDefault<string, 'contain'>` / `WithDefault<string, 'model'>`,
keep the validation in the adapters exactly as it is today, and keep the
`RlottieResizeMode` / `RlottieCacheStrategy` unions in the *public* `src/types.ts`
where they belong. We give up a generated C++ enum and compile-time
exhaustiveness in return for not introducing an app-kill path into a library
whose entire posture is "untrusted input, fail typed, never terminate the
process" (plan §16, and `cpp/FrameBuffer.h`'s `std::length_error` fix in Chunk
7.3 for exactly this reason).

### 3.2.4 `progress` has no "unset" and does not need one

Legacy iOS seeks on every `setProgress:` call (`RNRlottieView.mm:509-515`);
Legacy Android likewise (`RlottieViewManager.kt:245-248`). Under Fabric there is
no "prop absent", only a default. Rather than introduce a `-1` sentinel — which
would change a frozen contract for a prop documented as `0..1` — the rule is:

**Seek only when `newProps.progress != oldProps.progress`.**

On first mount `oldProps` is the default-constructed props, so `progress` is
`0.0` on both sides and no seek is issued. The only behavioural difference from
Legacy is a consumer who passes an explicit `progress={0}` on mount and expects
an explicit seek-to-0; that is indistinguishable from the default and is a no-op
in practice (frame 0 with `startFrame` 0). Documented, not silently different.

This same "apply on change vs `oldProps`" rule is what replaces the Legacy
path's hand-built dirty-flag coalescing, and it is strictly more precise: the
Legacy iOS view sets `_configureDirty = YES` in six setters and flushes in
`didSetProps:`; Fabric hands us both prop objects and lets us compare.

### 3.2.5 The rest

| prop | spec type | iOS `Props` field | Android generated setter |
| --- | --- | --- | --- |
| `autoPlay` | `WithDefault<boolean, false>` | `bool` | `setAutoPlay(view, boolean)` |
| `loop` | `WithDefault<boolean, false>` | `bool` | `setLoop(view, boolean)` |
| `repeatCount` | `WithDefault<Int32, 0>` | `int` | `setRepeatCount(view, int)` |
| `speed` | `WithDefault<Double, 1.0>` | `double` | `setSpeed(view, double)` |
| `progress` | `WithDefault<Double, 0.0>` | `double` | `setProgress(view, double)` |
| `startFrame` | `WithDefault<Int32, 0>` | `int` | `setStartFrame(view, int)` |
| `endFrame` | `WithDefault<Int32, 0>` | `int` | `setEndFrame(view, int)` |
| `resizeMode` | `WithDefault<string, 'contain'>` | `std::string` | `setResizeMode(view, String)` |
| `renderScale` | `WithDefault<Double, 1.0>` | `double` | `setRenderScale(view, double)` |
| `pauseWhenInactive` | `WithDefault<boolean, true>` | `bool` | `setPauseWhenInactive(view, boolean)` |
| `cacheStrategy` | `WithDefault<string, 'model'>` | `std::string` | `setCacheStrategy(view, String)` |
| `metricsEnabled` | `WithDefault<boolean, false>` | `bool` | `setMetricsEnabled(view, boolean)` |

`Double` rather than `Float` throughout, matching the current wire types.
Defaults are copied from `docs/bridge-contract.md`'s Props table and must stay
equal to the defaults in `src/RlottieView.tsx` — which resolves every optional
before it reaches native precisely so the documented and native defaults cannot
drift. Because `RlottieView.tsx` keeps doing that, the codegen defaults are in
practice never exercised; they exist so a raw spec consumer gets the documented
behaviour.

## 3.3 Commands

Under Fabric, commands are dispatched **by name only**. The generated JS helper
`codegenNativeCommands` calls `dispatchCommand(ref, command, args)`
(`node_modules/react-native/Libraries/Utilities/codegenNativeCommands.js:20-27`),
which branches on `global.RN$Bridgeless` and forwards the command **string**
either to `ReactFabric.dispatchCommand` or `ReactNative.dispatchCommand`
(`Libraries/ReactNative/RendererImplementation.js:82-102`). No integer index is
involved anywhere.

**The numeric ids in `docs/bridge-contract.md` are therefore untouched and
remain authoritative for the Legacy path only.** They must still never be
renumbered, `__reservedCommandSlot0` must stay at declaration index 0 in
`ios/RNRlottieViewManager.mm`, and `playMarker` must stay last there. None of
that applies to Fabric, and the Fabric component view must not grow a reserved
slot or any declaration-order sensitivity — adding one would be cargo-culting a
constraint that does not exist on this path.

### iOS

Codegen emits `RCTRlottieViewViewProtocol` with one method per command and an
inline dispatcher `RCTRlottieViewHandleCommand(componentView, commandName, args)`
(`GenerateComponentHObjCpp.js:26-83, 219-296`). Selectors follow "first
parameter unlabelled":

```objc
- (void)play:(NSInteger)startFrame endFrame:(NSInteger)endFrame;
- (void)pause;
- (void)seekToProgress:(double)progress;
- (void)playMarker:(NSString *)name;
```

The component view implements `-handleCommand:args:` as a single call into the
generated dispatcher. Two things to know about that dispatcher:

- Arg **count** and arg **type** validation are both inside `#if RCT_DEBUG`
  (`GenerateComponentHObjCpp.js:41-46, 57-62`). In a release build a
  short `args` array indexes past the end and raises `NSRangeException`. So JS
  must always send exactly the declared number of arguments — which it does,
  because `src/RlottieView.tsx` already passes `[start, end]` for `play` with
  `-1` sentinels rather than a variadic list.
- An unknown command name only `RCTLogError`s in debug and is otherwise a
  silent no-op, which matches the contract's "commands to a view without a live
  handle are dropped silently".

### Android

The generated `RlottieViewManagerInterface<RlottieView>` declares the typed
command methods, and `RlottieViewManagerDelegate.receiveCommand(view, String, args)`
decodes and dispatches to them. Design rule for `RlottieViewManager.kt`:

- Implement the interface's typed methods as the **real bodies** (`play(view,
  start, end)` → `view.play(start, end)`, and so on).
- Keep `override fun receiveCommand(root, commandId: Int, args)` mapping the
  frozen ids 1–9 onto those same typed methods — the Legacy integer path.
- Keep `override fun receiveCommand(root, commandId: String, args)` handling the
  stringified-numeric case that some RN versions send, and otherwise delegating
  to `super.receiveCommand(...)` so the generated delegate does the typed decode.
  Overriding the String form without that `super` call would bypass the delegate
  entirely on Fabric, which works but duplicates the decode; delegating keeps
  one decoder.

### `src/commands.ts`

`dispatchRlottieCommand` keeps its contract — never throws, returns `false` for
an unmounted view — and gains one architecture branch:

```
if (new architecture) -> Commands[name](ref, ...args)   // from the spec file
else                  -> existing UIManager path        // unchanged
```

wrapped in the existing `try`/`catch` and behind the existing
`resolveNativeTag`-style null guard.

Why not just use `Commands` everywhere and delete the UIManager path:
`codegenNativeCommands` → `RendererProxy.dispatchCommand` requires a mounted
host instance and will throw on a stale ref, and `src/commands.ts` exists
specifically to be robust across RN 0.68–0.81 — routing a 0.68 app through a
0.81-era helper is a regression risk for zero benefit. The new-architecture path
only has to work on >= 0.76, where the generated `Commands` object is the
supported mechanism. Cost of keeping both: one `if`.

`RLOTTIE_COMMAND_IDS` and the `RlottieCommand` enum stay exported for reference
and stay out of the dispatch path, for the reason already recorded in that
file's comments.

## 3.4 Events

All seven events are `DirectEventHandler` — no bubbling, matching the Legacy
`getExportedCustomDirectEventTypeConstants` / `RCTDirectEventBlock` surface.

`DirectEventHandler<null>` is the sanctioned form for a no-payload event and is
explicitly handled by the parser, which returns `argumentProps: []`
(`node_modules/@react-native/codegen/lib/parsers/parsers-commons.js:1107-1114`).
That is how the four lifecycle events keep their `{}` payload.

Nested arrays-of-objects in event payloads are supported, so
`onAnimationLoaded`'s `markers` array works
(`GenerateEventEmitterH.js:120-160`, `GenerateEventEmitterCpp.js:186-300`).

### 3.4.1 `onMetrics`: the throttle, and fixing the int/uint64 divergence

The `metricsEnabled` gate and the **<= 1/sec** throttle stay exactly where they
are: in `RNRlottieView.mm`'s `-onDisplayLinkTick:` (`ios/RNRlottieView.mm:278-337`)
and `RlottieView.kt`'s `onFrameTick` (`android/.../RlottieView.kt:403-495`).
Neither the Fabric component view nor the Fabric-aware ViewManager may emit
`onMetrics` on its own schedule. `onMetrics` remains the only recurring event in
the library and must not become an `onFrame` in disguise — see
`docs/bridge-contract.md`'s Phase 7 section, which is explicit that the throttle
lives in the contract and not only in the code.

**This is the right moment to fix the known payload divergence, and the design
says fix it.** CLAUDE.md records that Android writes `framesRendered`,
`framesDropped`, `bufferAllocCount`, `peakBufferBytes` with
`WritableMap.putInt` (32-bit, saturating via `coerceAtMostInt`,
`RlottieView.kt:693`) while iOS boxes the native `uint64_t` exactly, and that
the honest fix is `putDouble` but it changes a wire type the contract froze as
`int`.

Decision: **declare all four counters plus `uiStallCount` as `Double` in the
spec, and change Legacy Android to `putDouble` in the same chunk.**

- Under Fabric the payload type is generated **once** from **one** spec for both
  platforms, so the divergence cannot exist on that path — the only question is
  which single type it is. `Int32` would truncate above 2^31 on both platforms,
  which is worse than today's saturate-on-one-platform.
- JS numbers are IEEE doubles and exact to 2^53, so no consumer sees a
  difference for any reachable value.
- `src/types.ts`'s `RlottieMetricsEvent` already types every field as `number`,
  so the public TS surface does not change at all.
- Leaving Legacy Android on `putInt` while Fabric Android is a double would
  create a *third* variant. Changing it is a two-line edit
  (`RlottieEvents.kt:82-86`) and is observationally identical in JS.

This is a **contract amendment**: `docs/bridge-contract.md`'s Phase 7 field
table must change `int` to `double` for `framesRendered`, `framesDropped`,
`bufferAllocCount`, `peakBufferBytes`, `uiStallCount`, with a note recording
that it was deliberate and why. The amendment lands in the same chunk as the
code, not later.

### 3.4.2 iOS emission

Codegen generates `RlottieViewEventEmitter` with one method per event, named
after the prop (`onAnimationLoaded(OnAnimationLoaded)`, `onAnimationStart()`),
which internally calls `dispatchEvent` and gets normalized to
`topAnimationLoaded` by `EventEmitter::normalizeEventType`
(`node_modules/react-native/ReactCommon/react/renderer/core/EventEmitter.cpp:29-39`).
The generated JS view config maps that back to the `onAnimationLoaded` prop, so
no naming work is needed on iOS.

The component view caches
`std::shared_ptr<const RlottieViewEventEmitter>` in `-updateEventEmitter:`,
clears it in `-prepareForRecycle` and on a nil update, and null-checks before
every emit.

**Events are emitted from the main thread only.** `EventEmitter::dispatchEvent`
is in fact safe from any thread — `EventQueue::enqueueEvent` is documented
"Can be called on any thread" and is mutex-guarded
(`ReactCommon/react/renderer/core/EventQueue.h:34`,
`EventQueue.cpp:31`) — so emitting straight from the render worker would compile
and probably work. **Reject it.** Two reasons: it would fork the
worker→main marshalling in `ios/RNRlottiePlayer.mm`'s `SinkBridge`
(`RNRlottiePlayer.mm:32-57`) that the Legacy path depends on, giving two
different event-delivery models to reason about; and the `_eventEmitter`
`shared_ptr` is written on main by `-updateEventEmitter:`, so reading it from
the worker is a data race on the pointer regardless of what the queue behind it
guarantees. Keep the existing hop; the Fabric view emits from the same
main-thread callback blocks the Legacy view uses.

### 3.4.3 Android emission — one path for both architectures

`RCTEventEmitter.receiveEvent` does not reach Fabric's event pipeline. The
replacement is arch-neutral, which means Android gets **one** event path rather
than two:

```kotlin
val dispatcher = UIManagerHelper.getEventDispatcherForReactTag(reactContext, view.id) ?: return
dispatcher.dispatchEvent(RlottieEvent(UIManagerHelper.getSurfaceId(view), view.id, name, payload))
```

`getEventDispatcherForReactTag` resolves correctly on both architectures
(`UIManagerHelper.kt:107-133`), and `getSurfaceId(view)` returns `-1` on the
legacy hierarchy (`UIManagerHelper.kt:179-191`), which is what makes the same
`Event` subclass work in both. `Event`'s base class handles the two delivery
shapes itself (`Event.kt:146` legacy `dispatch(RCTEventEmitter)`, `Event.kt:186`
`dispatchModern(RCTModernEventEmitter)`).

Two mandatory details:

- **`override fun canCoalesce(): Boolean = false` on every event.** `Event`
  defaults to `canCoalesce() = true` with `getCoalescingKey() = 0`
  (`Event.kt:88, 106`), and `EventDispatcherImpl` coalesces on
  `(viewTag, eventName, coalescingKey)` (`EventDispatcherImpl.kt:148-165`). Two
  `onAnimationLoop` events in one batch would silently collapse into one — a
  dropped loop notification, with no error anywhere. The Legacy
  `RCTEventEmitter` path never coalesced, so this is a regression that would be
  introduced by the migration itself, and it is exactly the kind of thing that
  shows up as "the loop counter is sometimes short" months later.
- **The event name must be the `top`-prefixed form, and
  `getExportedCustomDirectEventTypeConstants` must register both keys.** Java-side
  dispatch does not go through C++ `normalizeEventType`, so the name we supply is
  the name the renderer looks up. Fabric's generated view config keys
  `directEventTypes` as `topAnimationLoaded`; the Legacy native view config is
  keyed by whatever we put in `getExportedCustomDirectEventTypeConstants`, which
  today is `onAnimationLoaded`. Registering **both** `onAnimationLoaded` and
  `topAnimationLoaded`, each with `registrationName = "onAnimationLoaded"`, and
  always dispatching `topAnimationLoaded`, makes one dispatch name correct on
  both architectures. The `top`-prefixed key is RN's own historical convention
  (`topChange` and friends), and the duplicate entry is harmless — both resolve to
  the same prop.

  The alternative — keep `RCTEventEmitter` for Legacy and use `EventDispatcher`
  only for Fabric — is rejected: two event paths on one platform is exactly the
  kind of asymmetry `docs/bridge-contract.md` was written to prevent, and the
  Legacy path would then be the only untested-under-Fabric code left in the
  file.

`RlottieView.kt`'s listener properties, the poll/drain sequence, the
`RlottieLoadedInfo`/`RlottiePlayerError`/`RlottieMetricsInfo` data classes, and
`RlottieEvents`' payload builders are all unchanged apart from the `putDouble`
change in §3.4.1. Only the final "how does this `WritableMap` reach JS" step
changes.

### 3.4.4 "No event after unmount" on both platforms

The invariant is unchanged and is enforced in the same places:

- iOS: the component view nils the child view's event blocks and calls
  `-teardown` (which joins the render worker and nils the player's own blocks)
  before releasing anything — the same ordering `RNRlottieView.mm:683-701`
  already implements. Then `_eventEmitter` is cleared. Fabric additionally
  disables the emitter's event target on unmount, so a late emit would be
  dropped anyway; the local null check is what keeps this a local invariant
  rather than a remote one.
- Android: `onDropViewInstance` already nils every listener **before** calling
  `view.release()` (`RlottieViewManager.kt:89-110`), and that order is
  load-bearing. Fabric calls `onDropViewInstance` too, so nothing changes.

## 3.5 Threading, settled

This is the section to read before writing any code.

### iOS — every Fabric callback we care about is on the main queue

Verified in `node_modules/react-native/React/Fabric/Mounting/RCTMountingManager.mm`:

| callback | thread | evidence |
| --- | --- | --- |
| `-updateProps:oldProps:` | main | reached from `performTransaction`, `RCTAssertMainQueue()` at `:250` and `:236` |
| `-updateState:oldState:` | main | same transaction |
| `-updateEventEmitter:` | main | same transaction |
| `-updateLayoutMetrics:oldLayoutMetrics:` | main | same transaction |
| `-finalizeUpdates:` | main | same transaction |
| `-mountChildComponentView:index:` / `-unmountChildComponentView:index:` | main | same transaction |
| `-handleCommand:args:` | main | `-dispatchCommand:` hops via `RCTExecuteOnMainQueue` at `:202-214` |
| `-prepareForRecycle` | main | `RCTComponentViewRegistry.mm:107, 116` |
| `+componentDescriptorProvider` | any (class method, no state) | — |

`Props` objects are **constructed** on the shadow/background thread during
`ShadowNode` cloning, but they are only ever *handed to a component view* on
main. So the component view never touches a prop object concurrently with its
construction.

Consequence: **every call the Fabric component view makes into `RNRlottiePlayer`,
`PlaybackController`, or `RenderCoordinator` is on the main thread — the exact
thread the Legacy adapter already guarantees** (`ios/RNRlottieView.h:16`,
`ios/RNRlottiePlayer.h:8-12`). The `[UI]`-only contract on
`PlaybackController` and the `[any]`-thread contract on `RenderCoordinator` both
continue to hold unchanged. No new synchronization is needed anywhere.

The one genuinely new thread interaction is the event emitter, and §3.4.2
resolves it by not introducing one.

### Android — mounting is on the UI thread, same as before

Fabric mounts through the **existing** `ViewManager`, so prop application and
command receipt happen where they always did. Specifically,
`ViewManager.updateProperties` calls `onAfterUpdateTransaction(view)` as its last
step (`.../uimanager/ViewManager.java:98-106`), and that method is invoked by
both `SurfaceMountingManager` (Fabric) and `NativeViewHierarchyManager`
(Legacy). So:

- **`onAfterUpdateTransaction` is called under Fabric**, and the coalesced
  single `configure(...)` flush in `RlottieViewManager.kt:227-240` keeps working
  with no change.
- `RlottieView.kt`'s Choreographer tick, the poll/drain/present sequence, the
  two-Bitmap swap, and the `handle != 0L` guards are all untouched. The
  `AndroidFrameSink` still never calls into the JVM, so there is still no
  `JavaVM` attach on the render worker and no weak-global-ref lifetime hazard —
  the two Android decisions CLAUDE.md flags as non-negotiable are simply not
  in scope for this work.

### The one Fabric behaviour that *does* change: full props per commit

Under Fabric the native side is handed a **complete** prop set on every commit
that touches the node, not a diff. Concretely, on Android
`SurfaceMountingManager` builds a `ReactStylesDiffMap` from the new
`ShadowView.props`, which is the full set, so **every `@ReactProp` setter runs on
every commit** — including commits caused by an unrelated style change.

This is the source of three real hazards, and mitigating them is not optional:

1. **`configure()` on every commit.** `PlaybackController::configure` sets
   `needsAnchor_ = true` when playing and `forceRender_ = true`
   (`cpp/PlaybackController.cpp:55-66`). Re-anchoring the monotonic clock on
   every commit discards accumulated sub-frame time and forces an extra render.
   Fix: the ViewManager must only set `config.dirty = true` when the value
   **actually changed** from the stored `ViewState` value, so
   `onAfterUpdateTransaction` issues `configure()` only on a real change.
2. **`seekToProgress()` on every commit.** `setProgress` currently applies
   unconditionally (`RlottieViewManager.kt:245-248`), which under Fabric would
   fight playback on every commit. Fix: store the last applied value in
   `ViewState` and compare.
3. **`setSource()` on every commit** — the worst of the three: it would
   re-run the resolver (file read, `content://` copy), bump the generation, and
   restart the animation. Fix: the `cacheKey`-based comparison from §3.2.1,
   with the last applied key held in `ViewState`.

The same three guards on iOS come free from the `oldProps`/`newProps`
comparison, which is why the iOS side of this is simply "compare before
applying" and the Android side needs explicit last-value tracking. Note the
guards are **also correct and harmless on the Legacy path** — RN sends only
changed props there, so the comparison just always says "changed" — which means
they go in unconditionally rather than behind an architecture check.

## 3.6 iOS ownership: the component view composes the existing view

Three options were considered:

- **A. The Fabric component view owns the existing `RNRlottieView` as its single
  child** and translates Fabric props/commands/events onto that view's existing
  public surface.
- **B. The component view owns an `RNRlottiePlayer` directly** and
  reimplements the display link, presenter, resize debounce, and lifecycle
  handling.
- **C. Extract a UIKit-only `RNRlottiePlayerSurface`** owning
  player + presenter + display link + lifecycle + resize, and have both
  `RNRlottieView` (Legacy) and the Fabric component view own one.

**Chosen: A.** Reasoning:

1. It requires **zero changes to `ios/RNRlottieView.mm`**, which is the shipped,
   device-verified Legacy path. B duplicates ~700 lines of subtle code; C edits
   the shipped file. The brief says Legacy keeps working unchanged, and A is the
   only option where that is true by construction rather than by test.
2. `RNRlottieView` is already RN-agnostic apart from `RCTDirectEventBlock`,
   which is a plain block typedef from `RCTComponent.h` with no bridge
   dependency — stated in its own header at `ios/RNRlottieView.h:21` and
   `:9-11`. So it composes under Fabric without dragging the Legacy bridge in.
3. It reuses, verbatim, every hard-won behaviour: the `NSProxy` weak
   display-link target that breaks the `CADisplayLink` retain cycle
   (`RNRlottieView.mm:42-81`), the 100 ms resize debounce (`:359-415`), the
   `_isPlayingIntent`/`_pausedByLifecycle` bookkeeping that never overrides an
   explicit JS pause (`:465-477`), the teardown ordering (`:683-701`), the
   zero-copy `CGImage` presentation (`:224-245`), and the `onMetrics` throttle
   and stall detection (`:278-337`). Re-deriving any of those is how a
   second-architecture port introduces bugs the first one already fixed.
4. `RNRlottieView` exposes exactly the seams Fabric needs: settable event
   blocks, a `player` accessor, `pendingSource`, the plain KVC prop setters, and
   `-didSetProps:` — which maps one-to-one onto `-finalizeUpdates:`.

Cost of A: one extra `UIView` in the hierarchy (no masking, no opacity, so
negligible), and the component view must forward its bounds to the child.

Option C remains the right long-term refactor and is explicitly deferred, so
that a future third adapter does not make it three-way duplication.

### `ios/fabric/RNRlottieComponentView.h/.mm` — the contract

Whole file inside `#ifdef RCT_NEW_ARCH_ENABLED`.

```objc
@interface RNRlottieComponentView : RCTViewComponentView <RCTRlottieViewViewProtocol>
@end
```

- `+ (ComponentDescriptorProvider)componentDescriptorProvider` returns
  `concreteComponentDescriptorProvider<RlottieViewComponentDescriptor>()`.
  **No custom `ComponentDescriptor`, `ShadowNode`, or `State`** — the generated
  concrete ones suffice, because the component needs no custom measurement and
  carries no native state back into the shadow tree.
- `-initWithFrame:` sets `_props = RlottieViewShadowNode::defaultSharedProps()`,
  creates the child `RNRlottieView`, adds it as a subview, and wires the child's
  seven event blocks to blocks that emit through `_eventEmitter` (null-checked).
- `-updateProps:oldProps:` static-casts both to
  `const RlottieViewProps &`, then applies **only what changed**, per §3.2/§3.4:
  scalar comparisons for the primitives, the `cacheKey` rule for `source`, the
  hand-written comparators for the three override arrays. Playback-shaping
  values are stored on the child (the existing setters mark it dirty); nothing
  is flushed here.
- `-finalizeUpdates:` calls `[_view didSetProps:@[]]` when the mask includes
  `RNComponentViewUpdateMaskProps`, which performs the single coalesced
  `configure(...)` and the pending-source flush exactly as the Legacy path does.
- `-updateLayoutMetrics:oldLayoutMetrics:` calls `super` (which sets
  `self.frame` from `layoutMetrics.frame`, in points), then sets
  `_view.frame = self.bounds`. That triggers the child's `-layoutSubviews`,
  which runs the existing `scheduleSurfaceResizeForBounds:` debounce
  unmodified. See §3.7.
- `-handleCommand:args:` calls `RCTRlottieViewHandleCommand(self, commandName, args)`;
  the nine protocol methods forward to `_view.player` exactly as
  `RNRlottieViewManager.mm` does today, minus the `addUIBlock:` hop (we are
  already on main).
- `-updateEventEmitter:` caches the typed emitter; a nil update clears it.
- `-prepareForRecycle` calls `super`, tears the child down completely
  (`[_view teardown]`), drops it, clears `_eventEmitter`, and resets `_props` to
  the default. A fresh child is created lazily on the next `-updateProps:`.
- `+ (BOOL)shouldBeRecycled { return NO; }`.

### Recycling: opt out, but still implement `prepareForRecycle` properly

`RCTComponentViewRegistry` maintains a recycle pool keyed by component handle;
on unmount it calls `-prepareForRecycle` and **keeps the object alive** for
reuse by an unrelated React element (`RCTComponentViewRegistry.mm:104-119`), and
`-dealloc` may simply never run. `RCTComponentViewFactory` honours an optional
`+shouldBeRecycled` class method to opt out
(`RCTComponentViewFactory.mm:109-110`).

Decision: **return `NO` for v1.** A pooled `RlottieView` would hold a live
render worker thread and its double frame buffers for as long as it sits in the
pool, multiplied by the pool size. That directly contradicts §21's memory
posture and the Chunk 7.3 lifecycle work. Opting out makes the Fabric lifecycle
isomorphic to the Legacy one — created on mount, destroyed on unmount — which is
also what makes the existing lifecycle tests meaningful for both paths.

`-prepareForRecycle` is **still** implemented as a full teardown, for two
reasons: `+shouldBeRecycled` is discovered via `respondsToSelector:` rather than
declared in a public protocol, so it is a soft contract that could change; and
if the flag is ever flipped, a partial reset would leak animation state across
unrelated React elements — the classic recycling bug, and a very confusing one
(an animation from a deleted list row appearing in a new one).

## 3.7 Resize

Legacy iOS computes `pixelW = bounds.width * screenScale * renderScale`, clamps
per axis to 4096, and **debounces 100 ms** before calling
`-setSurfaceWidth:height:` (`ios/RNRlottieView.mm:19-25, 359-415`). Legacy
Android computes from `View.getWidth()` — which is **already physical pixels**,
the platform trap CLAUDE.md records — times `renderScale`, clamps to the pixel
area budget, and debounces 80 ms (`RlottieView.kt:570-604`).

Under Fabric:

- **iOS**: `-updateLayoutMetrics:oldLayoutMetrics:` supplies
  `layoutMetrics.frame` in **points** (same units as `UIView.bounds`, so the
  `* screenScale` multiplication stays correct) plus `pointScaleFactor`. Calling
  `super` then assigning `_view.frame = self.bounds` reuses the child's
  `-layoutSubviews` path with no new resize logic at all. Keep using the
  window's `screen.scale` as the child already does rather than
  `layoutMetrics.pointScaleFactor`; the child is in the same window, and mixing
  two scale sources is how a half-resolution surface bug gets introduced.
- **Android**: `onSizeChanged` fires under Fabric exactly as before, because
  Fabric sets the view's layout through the same `View` APIs. Nothing changes.

**The debounce becomes more important, not less.** Fabric can deliver a
layout-metrics update on every commit, so a resize storm is at least as likely
as under Legacy. Do not remove or shorten either debounce, and do not "optimize"
by applying the size synchronously in `-updateLayoutMetrics:` — that would
allocate frame buffers and bump the generation once per commit during an
interactive layout animation, which is precisely what the debounce exists to
prevent.

## 3.8 JS host component

`src/RlottieNativeComponent.ts` currently calls
`requireNativeComponent<RlottieNativeProps>('RlottieView')`. It is replaced by a
re-export of the spec's default export.

That is safe on the Legacy path, which is not obvious and is worth recording:
the babel plugin rewrites `codegenNativeComponent` into
`NativeComponentRegistry.get(name, viewConfigProvider)`, and on a
non-bridgeless runtime `NativeComponentRegistry.get` resolves
`native: !global.RN$Bridgeless` → `true` and therefore prefers
`getNativeComponentAttributes(name)` — the **native** view config from the
Legacy ViewManager — using the static config only as a fallback
(`node_modules/react-native/Libraries/NativeComponent/NativeComponentRegistry.js:57-70`).
Static-config *validation* against the native config only runs when the runtime
config provider asks for it (`verify`), which is not the default. So an
old-architecture app keeps using exactly the view config it uses today, and a
spec/native mismatch cannot silently change Legacy behaviour.

`RlottieNativeProps` stays as the internal prop type used by
`src/RlottieView.tsx`; it must stay structurally compatible with the spec's
`NativeProps`. `src/RlottieView.tsx` itself needs **no logic change**: the
default resolution, the render-time error dedup keyed on failure content, the
stable-source-identity `useRef`, and the `useImperativeHandle` ref methods all
work unchanged.

---

# 4. What changes, file by file

## Unchanged — the shared core is genuinely architecture-neutral

| file | why untouched |
| --- | --- |
| `cpp/RlottiePlayerCore.{h,cpp}` | never sees a platform type; `[worker]`-only contract unaffected |
| `cpp/RenderCoordinator.{h,cpp}` | `[any]`-thread public API already covers every Fabric caller |
| `cpp/FrameSink.h` | both adapters keep their existing sinks |
| `cpp/FrameBuffer.{h,cpp}`, `cpp/PlaybackController.{h,cpp}` | `[UI]`/`[worker]` contracts hold; Fabric callbacks are all UI-thread |
| `cpp/Metrics.h`, `cpp/InputLimits.h`, `cpp/CacheKey.h`, `cpp/ModelCacheController.{h,cpp}`, `cpp/ErrorCode.h`, `cpp/PixelFormat.h`, `cpp/Sha256.h`, `cpp/AnimationSource.h`, `cpp/AnimationMetadata.h`, `cpp/PlayerError.h`, `cpp/PlaybackState.h`, `cpp/RlottieVersion.h` | pure C++, no RN dependency |
| `cpp/third_party/rlottie/**` | pinned vendored engine |
| `android/src/main/cpp/**` (`RlottieJni.cpp`, `JniPlayerHandle.*`, `AndroidFrameSink.*`, `AndroidPixelConvert.h`, `AndroidEventEncoding.h`, `CMakeLists.txt`, `react-native-rlottie.expmap`) | Fabric mounts through the existing Java ViewManager; the codegen C++ lives in a separate `.so` built by the app |
| `ios/RNRlottieView.{h,mm}` | composed as-is by the Fabric component view (§3.6) |
| `ios/RNRlottiePlayer.{h,mm}` | owned identically by both adapters |
| `ios/RNRlottieFramePresenter.{h,mm}` | presentation is arch-independent |
| `ios/RNRlottieSourceResolver.{h,mm}`, `android/.../RlottieSourceResolver.kt` | still fed an `NSDictionary` / `ReadableMap` |
| `ios/RNRlottieViewManager.mm` | the Legacy ViewManager; **must not be edited**, or the numeric command ids shift |
| `src/RlottieView.tsx`, `src/source.ts`, `src/sha256.ts`, `src/types.ts` | no logic change |
| `tests/cpp/**`, `tests/js/**` | core and JS-normalization tests unaffected |

## New files

| path | contents |
| --- | --- |
| `src/specs/RlottieViewNativeComponent.ts` | Fabric component spec + `Commands` (§3.1) |
| `src/specs/NativeRlottieModule.ts` | TurboModule spec (§2.2) |
| `ios/fabric/RNRlottieComponentView.h` | `RCTViewComponentView` subclass declaration, inside `#ifdef RCT_NEW_ARCH_ENABLED` |
| `ios/fabric/RNRlottieComponentView.mm` | the whole iOS Fabric adapter (§3.6) |
| `android/src/main/java/com/rlottie/RlottieEvent.kt` | one `Event<RlottieEvent>` subclass, `canCoalesce() = false` (§3.4.3) |
| `docs/new-architecture-design.md` | this document |

## Modified files

| path | change | risk |
| --- | --- | --- |
| `package.json` | add `codegenConfig`; add `src/specs` assertions to the package-verify script | low |
| `react-native.config.js` | pin `libraryName`, `componentDescriptors`, `cmakeListsPath` | low |
| `react-native-rlottie.podspec` | `require react_native_pods`; add `ios/fabric/**` to `core`'s `source_files`; `install_modules_dependencies(s)` **last**; drop the now-ineffective `c++17` pin | **medium** — C++20 for the core, pod graph growth |
| `android/build.gradle` | `apply plugin: "com.facebook.react"` + buildscript classpath + `react { reactNativeDir }` | **medium** — must keep the standalone build working |
| `scripts/check-android-build.sh` | supply what the RN Gradle plugin needs standalone | medium |
| `ios/RNRlottieModule.mm` | conform to the generated spec protocol, add `getTurboModule:`, switch `RCT_REMAP_METHOD` → `RCT_EXPORT_METHOD` with codegen selectors | medium — selector names must match generated output exactly |
| `android/.../RlottieModule.kt` | extend `NativeRlottieModuleSpec`; drop `getName()`/`NAME` | low |
| `android/.../RlottiePackage.kt` | `BaseReactPackage` with `getModule` + `getReactModuleInfoProvider` | low |
| `android/.../RlottieViewManager.kt` | implement `RlottieViewManagerInterface`, add `getDelegate()`, add the three change-guards from §3.5, route the String `receiveCommand` through `super` | **high** — the change-guards are correctness-critical and easy to get subtly wrong |
| `android/.../RlottieEvents.kt` | dispatch via `EventDispatcher`; register both `onX` and `topX` keys; `putInt` → `putDouble` for the five counters | medium |
| `android/.../RlottieView.kt` | `RlottieMetricsInfo` counters `Int` → `Double`; drop `coerceAtMostInt` | low |
| `src/RlottieNativeComponent.ts` | re-export the spec's component; keep `RlottieNativeProps` | low |
| `src/commands.ts` | add the new-architecture dispatch branch (§3.3) | low |
| `src/RlottieModule.ts` | resolve through the spec's default export | low |
| `docs/bridge-contract.md` | Phase 7 table `int` → `double`; a new "New Architecture" section stating that numeric command ids are Legacy-only | low |
| `docs/api-reference.md`, `README.md`, `CHANGELOG.md`, `CLAUDE.md` | document dual-arch support and the RN >= 0.76 floor for it | low |
| `example/ios/Podfile`, `example/android/gradle.properties` | make the architecture togglable instead of hardcoded off | low |

---

# 5. Phased implementation plan

Ordering principle, which differs from the brief's suggested order and is
deliberate: **the specs are the contract both platforms generate from, so
nothing can be built before the schema exists**; and **Android is cheaper than
iOS** (no new native view, no new C++, no new build system integration beyond a
plugin), so Android validates the generated schema end-to-end fastest and finds
spec mistakes before they are baked into a 500-line Obj-C++ file. Build
integration comes second on each platform because a spec that does not generate
is not a spec.

### Chunk 9.1 — Codegen specs + package plumbing
| | |
|---|---|
| **Goal** | The two spec files and `codegenConfig` exist and produce correct artifacts on both platforms. |
| **Depends on** | — |
| **Deliverables** | `src/specs/RlottieViewNativeComponent.ts`, `src/specs/NativeRlottieModule.ts`, `codegenConfig` in `package.json`, pinned `react-native.config.js`, package-verify assertions. |
| **Contract** | Spec content per §3.1/§2.2. Filenames are load-bearing (§1.1). `type: "all"`. `resizeMode`/`cacheStrategy` are `string`, not unions (§3.2.3). Metrics counters are `Double` (§3.4.1). Lifecycle events are `DirectEventHandler<null>`. |
| **Acceptance** | `npx @react-native-community/cli codegen` (or the RN 0.76 equivalent) produces, and the chunk **reads and records**: `RlottieViewComponentDescriptor`, `RlottieViewProps` with the field types this document claims, `RlottieViewEventEmitter` with seven methods, `RCTRlottieViewViewProtocol` with nine command selectors, `RlottieViewManagerInterface`/`Delegate` in `com.facebook.react.viewmanagers`, `NativeRlottieModuleSpec` in `com.rlottie.spec`. Any divergence from §3 is reported and this document amended **before** any platform chunk starts. `npm run typecheck` and `npm run lint` pass. |
| **Agent** | RNEngineer |
| **Risks** | `Readonly<{}>`-style edge cases and the exact generated selector/method names are the two things most likely to differ from this document. Empty-payload events are handled by the parser (`parsers-commons.js:1107-1114`) but that path is worth eyeballing in the output. |

**Status: done.** `src/specs/RlottieViewNativeComponent.ts` and
`src/specs/NativeRlottieModule.ts` exist; `codegenConfig` and
`react-native.config.js` are in place; `scripts/verify-npm-package.sh` asserts
both spec files are packed. Verified by running RN's vendored
`scripts/generate-codegen-artifacts.js` directly (the `@react-native-community/cli`
binary is not installed in this environment and `npx` cannot reach the
registry here — the same known proxy limitation CLAUDE.md records elsewhere).
Output matched this document's predictions for `RlottieViewComponentDescriptor`,
`RlottieViewProps`, `RlottieViewEventEmitter`, `RCTRlottieViewViewProtocol`,
`RlottieViewManagerInterface`/`Delegate`, and `NativeRlottieModuleSpec`/
`NativeRlottieModuleSpecJSI`.

Two divergences from the §3.1 sketch, found only by actually running codegen
(the sketch there is explicitly "abbreviated"), now fixed in the real spec
file and worth recording so a future edit to that file doesn't reintroduce them:

- **Event payload arrays must use plain `T[]`, not `ReadonlyArray<T>`.**
  `@react-native/codegen`'s event-parsing path (`events.js`'s
  `getPropertyType`) switches on `TSArrayType` and has no case for the
  `ReadonlyArray` type-reference name, so `markers: ReadonlyArray<{...}>` on
  `onAnimationLoaded` throws "Unable to determine event type" at codegen time.
  Note this is an events-only constraint — `ReadonlyArray<T>` is fine for
  props (`colorOverrides` etc. keep using it).
- **An event array's element type must be an inline object literal, not a
  named type alias.** `extractArrayElementType` resolves an inline
  `TSTypeLiteral` but does not look up a referenced alias by name, so a
  separate `type Marker = {...}` used as the element type throws "Unrecognized
  Marker for Array markers in events". `onAnimationLoaded`'s `markers` element
  type is therefore inlined directly in the spec rather than pulled out as its
  own named type.

One environment note for Chunk 9.2: the standalone `generate-codegen-artifacts.js`
path hardcodes the TurboModule spec's Java package to
`com.facebook.fbreact.specs` regardless of `codegenConfig.android.javaPackageName`
— only the real Gradle plugin's `GenerateCodegenArtifactsTask.kt` honours that
setting. Not a defect in the spec files; `com.rlottie.spec` can only be
confirmed end-to-end once 9.2 wires up the actual Gradle build.

### Chunk 9.2 — Android build integration
| | |
|---|---|
| **Goal** | Codegen runs for this library under Gradle, on both architectures, and the standalone build still produces an AAR. |
| **Depends on** | 9.1 |
| **Deliverables** | `android/build.gradle`, `scripts/check-android-build.sh`. |
| **Contract** | §1.5. Generated Java on the main source set with **no** `src/newarch`/`src/oldarch` split. `android/src/main/cpp/**` and the expmap untouched. |
| **Acceptance** | `scripts/check-android-build.sh --link` still passes and the `Java_com_rlottie_*` symbols are still exported. `build/generated/source/codegen/{java,jni}` both populated. A standalone build still yields an AAR containing every Kotlin class plus both ABIs' `.so`. |
| **Agent** | RNEngineer |
| **Risks** | The standalone build is the likely casualty; it has broken silently before (Chunk 3.5). |

### Chunk 9.3 — Android TurboModule + package
| | |
|---|---|
| **Goal** | `RlottieModule` works as a TurboModule and as a legacy module from one class. |
| **Depends on** | 9.2 |
| **Deliverables** | `RlottieModule.kt`, `RlottiePackage.kt`. |
| **Contract** | §2.4. All three methods stay Promise-based. Bodies unchanged. `BaseReactPackage`, `isTurboModule = true`. |
| **Acceptance** | `configure`/`clearModelCache`/`getNativeVersion` resolve identically with `newArchEnabled` true and false; `getNativeVersion` returns the pinned SHA in both. |
| **Agent** | RNEngineer |

### Chunk 9.4 — Android event dispatch unification + metrics wire type
| | |
|---|---|
| **Goal** | One event path for both architectures, and the int/uint64 divergence closed. |
| **Depends on** | 9.2 |
| **Deliverables** | `RlottieEvent.kt` (new), `RlottieEvents.kt`, `RlottieViewManager.kt` (emit call sites), `RlottieView.kt` (`RlottieMetricsInfo` types), `docs/bridge-contract.md` amendment. |
| **Contract** | §3.4.1 and §3.4.3. `canCoalesce() = false` on every event. Dispatch `topX`; register both `onX` and `topX`. `putDouble` for the five counters. |
| **Acceptance** | All seven events arrive with identical payloads on both architectures. A dedicated test/manual check proves **two `onAnimationLoop` events in one batch both arrive** — the coalescing trap. `onMetrics` still fires at most once per second and not at all when `metricsEnabled` is false. |
| **Agent** | RNEngineer |
| **Risks** | Silent event loss is the failure mode; only an explicit two-in-one-batch check catches it. |

### Chunk 9.5 — Android Fabric component
| | |
|---|---|
| **Goal** | `RlottieView` mounts and behaves identically under Fabric. |
| **Depends on** | 9.4 |
| **Deliverables** | `RlottieViewManager.kt`. |
| **Contract** | §3.3 (commands), §3.5 (the three change-guards). Implement `RlottieViewManagerInterface`, return `RlottieViewManagerDelegate` from `getDelegate()`, **keep** the `@ReactProp` annotations (Legacy's native view config is reflected from them), keep both `receiveCommand` overloads. `RlottieView.kt` gains no new behaviour. |
| **Acceptance** | Every prop, all nine commands, all seven events verified under Fabric. Explicitly: an unrelated style-only re-render does **not** re-resolve `source`, does **not** re-issue `configure()`, and does **not** seek — the §3.5 hazards. Playback timing under repeated commits matches Legacy. |
| **Agent** | RNEngineer |
| **Risks** | Highest-risk Android chunk. The change-guards are the whole game; without them the animation restarts on unrelated renders, which will look like "Fabric is broken" rather than "a guard is missing". |

### Chunk 9.6 — iOS build integration
| | |
|---|---|
| **Goal** | The podspec supports both architectures and the codegen artifacts are reachable. |
| **Depends on** | 9.1 |
| **Deliverables** | `react-native-rlottie.podspec`. |
| **Contract** | §1.4. `install_modules_dependencies(s)` last. Fabric sources always in `source_files`, guarded by `#ifdef RCT_NEW_ARCH_ENABLED`. |
| **Acceptance** | `pod install` and a full app build succeed with `RCT_NEW_ARCH_ENABLED` both `0` and `1`. **rlottie still compiles** despite the C++20 switch (its per-file `-std=gnu++14` must still win). The shared core compiles clean as C++20. |
| **Agent** | RNEngineer, with CPPEngineer on the C++20 question |
| **Risks** | The C++20 promotion of the shared core is the one change here that touches shipped code semantics. |

### Chunk 9.7 — iOS TurboModule
| | |
|---|---|
| **Goal** | `RNRlottieModule` satisfies the generated protocol and works on both architectures. |
| **Depends on** | 9.6 |
| **Deliverables** | `ios/RNRlottieModule.mm`. |
| **Contract** | §2.3. Selectors must match the generated header, verified against the real file rather than this document. Bodies unchanged. |
| **Acceptance** | Same three calls resolve identically on both architectures; parity with Android's error contract preserved. |
| **Agent** | RNEngineer |

### Chunk 9.8 — iOS Fabric component view
| | |
|---|---|
| **Goal** | A Fabric component view that owns the existing `RNRlottieView` and reproduces the frozen contract. |
| **Depends on** | 9.6, 9.7 |
| **Deliverables** | `ios/fabric/RNRlottieComponentView.h/.mm`. |
| **Contract** | §3.6 in full. Composition, not reimplementation — **`ios/RNRlottieView.mm` must show a zero diff**. `-finalizeUpdates:` → `didSetProps:`. `-updateLayoutMetrics:` → child frame → existing debounce. Emitter cached on main, null-checked, emitted from main only. `+shouldBeRecycled` → `NO`, `-prepareForRecycle` → full teardown anyway. |
| **Acceptance** | All 13 props, nine commands, seven events verified under Fabric. Mount/unmount repeatedly with no leak (`tests/run-tests.sh leaks` still 0, plus an on-device pass). No event after unmount. Resize keeps the last frame. Background/foreground pause-resume matches Legacy. `git diff --stat ios/RNRlottieView.mm` is empty. |
| **Agent** | RNEngineer, with CPPArchitect/CPPEngineer reviewing the ownership and emitter-lifetime boundary |
| **Risks** | Largest new-code chunk. The recycling and emitter-lifetime hazards are the ones that produce late, confusing bugs rather than immediate failures. |

### Chunk 9.9 — JS layer
| | |
|---|---|
| **Goal** | The public JS surface drives whichever architecture is active. |
| **Depends on** | 9.5, 9.8 |
| **Deliverables** | `src/RlottieNativeComponent.ts`, `src/commands.ts`, `src/RlottieModule.ts`. |
| **Contract** | §3.8, §3.3, §2.5. `dispatchRlottieCommand` keeps never-throws and returns-false-for-unmounted on both branches. `dispatchRlottieCommand` stays unexported; `RlottieCommand` stays reference-only. `src/index.ts` and every public type unchanged. |
| **Acceptance** | `npm run test:js`, `npm run typecheck`, `npm run lint` pass. `ref.play()` racing mount is still safe on both architectures. |
| **Agent** | RNEngineer |

### Chunk 9.10 — Dual-architecture example app, verification matrix, docs
| | |
|---|---|
| **Goal** | Both architectures demonstrably work on device, and the docs say what is supported. |
| **Depends on** | 9.9 |
| **Deliverables** | `example/` toggle, a device verification matrix doc, updates to `docs/bridge-contract.md`, `docs/api-reference.md`, `README.md`, `CHANGELOG.md`, `CLAUDE.md`. |
| **Contract** | Four configurations: iOS old, iOS new, Android old, Android new. Golden-frame verification (`scripts/run-golden-*.sh`) run under the new architecture too — pixel output must be identical, since the render path is literally the same code. |
| **Acceptance** | All four configurations run the example app; the frozen contract holds in all four; goldens match. |
| **Agent** | RNEngineer |
| **Risks** | Genuinely needs hardware, like Chunk 7.4. Cannot be closed headlessly. |

---

# 6. Explicitly out of scope for v1 of this work

Each of these is a deliberate exclusion, not an oversight.

- **RN < 0.76 on the new architecture.** Two iOS registration mechanisms and a
  pre-bridgeless configuration matrix, for apps that overwhelmingly have not
  migrated. The Legacy path still covers 0.68+.
- **Bridgeless-specific APIs.** No direct use of `RCTHost`, `ReactHost`,
  `ReactHostDelegate`, or the bridgeless-only module lookups. Everything goes
  through codegen-generated registration, which works in both bridge-Fabric and
  bridgeless configurations. We do not ship a bridgeless-only code path.
- **Fabric synchronous layout / measurement.** No custom
  `LayoutableShadowNode::measureContent`, no intrinsic content size, no
  `YogaLayoutableShadowNode` customization. The component has no measurable
  content in the layout sense; the composition's authored size is reported to JS
  via `onAnimationLoaded` and consumers size the view themselves, exactly as
  under Legacy. Adding measurement would put animation metadata on the shadow
  thread, which is a new threading surface for no product benefit.
- **A custom C++ `ComponentDescriptor`, `ShadowNode`, or `State`.** The
  generated concrete ones suffice. In particular there is **no** Fabric `State`:
  nothing native needs to push data back into the shadow tree, and adding State
  would mean touching the shadow thread.
- **iOS component-view recycling.** `+shouldBeRecycled` returns `NO` (§3.6).
  Revisit only with a measurement showing mount cost matters.
- **RN's legacy interop layer** (`LegacyViewManagerInteropComponentDescriptor`).
  Ruled out before this document.
- **Removing or deprecating the Legacy path.** It stays until the supported RN
  floor moves past old-architecture support. Two paths is the cost of the
  decision at the top of this document.
- **Any change to `cpp/`.** If a chunk needs one, the design is wrong.
- **Fixing the pre-existing `resizeMode` / `cacheStrategy` no-ops on Android.**
  Both remain accepted-and-validated-but-inert there (CLAUDE.md's known
  follow-ups). This work must not make them *worse* — in particular the
  validation must keep rejecting silently rather than crashing (§3.2.3) — but
  implementing them is a draw-path/cache-flag change, unrelated to architecture.
- **`allowRemoteSources` and remote loading.** Still a JS-only gate; still
  `INVALID_SOURCE` either way; still no network I/O anywhere native. v1.1.
- **Alpha in `colorOverrides`.** Still discarded, on both architectures, because
  the core's `setColor` has no alpha parameter.
- **`onFrame`, or any per-frame bridge traffic.** Fabric's event pipeline is
  faster than the bridge's, which makes this temptation *more* available, not
  less. `onMetrics` at <= 1/sec remains the only recurring event.
- **Extracting `RNRlottiePlayerSurface`** (Option C in §3.6). The right
  long-term shape, deliberately deferred so this work does not edit the shipped
  Legacy view.
- **`react-test-renderer` coverage of `RlottieView`'s render-time behaviour.**
  Still open from Phase 4, still a version-pinning problem, still unrelated to
  architecture.
- **A CI matrix building all four configurations.** Chunk 9.10 establishes the
  manual matrix. Automating it needs device infrastructure this project does not
  have.

---

# 7. Ranked risks

By cost of getting it wrong, most expensive first.

1. **Missing the §3.5 change-guards on Android.** Fabric delivers full props per
   commit, so an unguarded `setSource` re-resolves the source and restarts the
   animation on every unrelated re-render, and an unguarded `configure()`
   re-anchors the playback clock. Presents as "Fabric playback is janky and
   restarts randomly" — a symptom several layers from the cause. Guard, and test
   with a style-only re-render.
2. **Modelling `resizeMode`/`cacheStrategy` as string-literal unions.** Generated
   enum conversion calls `abort()` on an unknown value
   (`GeneratePropsH.js:66-74`). An app-kill from an untyped JS prop, on a
   background thread, in a library whose stated posture is never to terminate the
   process. Use `string`.
3. **Forgetting `canCoalesce() = false` on Android events.** Silent, intermittent
   event loss (`EventDispatcherImpl.kt:148-165`). Nothing logs. Only an explicit
   two-events-in-one-batch test finds it.
4. **iOS component-view recycling.** State leaking from a deleted element into a
   new one, or a pooled view holding a render worker and its buffers. Opt out
   **and** implement `prepareForRecycle` fully.
5. **Emitting events from the render worker on iOS.** Compiles, mostly works,
   races the `_eventEmitter` `shared_ptr`. Keep the existing main-thread hop.
6. **Registration typos.** A wrong `componentProvider` class name makes
   `NSClassFromString` return nil inside an `NSDictionary` literal — the app
   throws at load. A missed `componentDescriptors` entry renders an
   unimplemented view with no error.
7. **The C++20 promotion of the shared core on iOS.** Believed harmless;
   unverified until 9.6 builds.
8. **Breaking the standalone Android build** by adding the RN Gradle plugin. Has
   happened before, silently.
9. **Codegen output differing from this document.** Mitigated by making 9.1
   read the real generated files and amend this document before any platform
   chunk starts.
