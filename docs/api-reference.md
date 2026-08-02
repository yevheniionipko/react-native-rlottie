# API reference

Everything here is derived from `src/index.ts` (the only supported import
path) and from `docs/bridge-contract.md`, which is the frozen authority for
prop/event/command names and payload shapes. If something here and the
contract doc ever disagree, the contract doc wins and this file has drifted.

```ts
import {
  RlottieView,
  Rlottie,
  configure,
  clearModelCache,
  getNativeVersion,
  isAvailable,
  RlottieCommand,
} from '@yevheniionipko/react-native-rlottie';
import type {
  RlottieSource,
  RlottieJsonObject,
  NormalizedRlottieSource,
  RlottieErrorCode,
  RlottieMarker,
  RlottieLoadedEvent,
  RlottieErrorEvent,
  RlottieLifecycleEvent,
  RlottieMetricsEvent,
  RlottieViewProps,
  RlottieResizeMode,
  RlottieCacheStrategy,
  RlottieColorOverride,
  RlottieOpacityOverride,
  RlottieStrokeWidthOverride,
  RlottieViewRef,
  RlottiePlayOptions,
  RlottieNativeVersion,
  RlottieConfigureOptions,
} from '@yevheniionipko/react-native-rlottie';
```

Importing from any other path (e.g.
`react-native-rlottie/src/commands`) reaches internal modules that can change
without notice and is not supported — `package.json`'s `exports` map enforces
this.

## `<RlottieView>` props

`RlottieView` extends RN's `ViewProps` (style, layout, accessibility, etc. all
work as usual). Every optional prop below is resolved to a concrete default in
`src/RlottieView.tsx` before it reaches native, so the defaults documented here
are exactly the ones the native side sees.

| prop                   | type                                            | default      | notes                                                                                                                                                                   |
| ---------------------- | ----------------------------------------------- | ------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `source`               | `RlottieSource`                                 | — (required) | See "Source forms" below.                                                                                                                                               |
| `allowRemoteSources`   | `boolean`                                       | `false`      | JS-only gate; see "Remote sources" below. Never crosses the bridge.                                                                                                     |
| `autoPlay`             | `boolean`                                       | `false`      |                                                                                                                                                                         |
| `loop`                 | `boolean`                                       | `false`      |                                                                                                                                                                         |
| `repeatCount`          | `number`                                        | `0`          | With `loop`: `0` is infinite, `N > 0` is N extra plays.                                                                                                                 |
| `speed`                | `number`                                        | `1.0`        | Negative plays in reverse, `0` holds the current frame. Non-finite values fall back to `1.0`.                                                                           |
| `progress`             | `number`                                        | —            | `0..1`, seeks without starting playback. Clamped to `[0, 1]`.                                                                                                           |
| `startFrame`           | `number`                                        | `0`          |                                                                                                                                                                         |
| `endFrame`             | `number`                                        | `0`          | `0` means "last frame".                                                                                                                                                 |
| `resizeMode`           | `'contain' \| 'cover' \| 'stretch' \| 'center'` | `'contain'`  | **iOS only.** Maps to `layer.contentsGravity`. Accepted and validated on Android but currently a **no-op** there — Android's draw path always stretches to view bounds. |
| `renderScale`          | `number`                                        | `1.0`        | Multiplies the native render-surface resolution.                                                                                                                        |
| `pauseWhenInactive`    | `boolean`                                       | `true`       | Pauses playback while the app is backgrounded.                                                                                                                          |
| `cacheStrategy`        | `'none' \| 'model'`                             | `'model'`    | **iOS only.** Plumbed into `useModelCache`. Accepted and validated on Android but currently a **no-op** there — Android's source natives take no cache flag yet.        |
| `colorOverrides`       | `RlottieColorOverride[]`                        | —            | See "Overrides" below.                                                                                                                                                  |
| `opacityOverrides`     | `RlottieOpacityOverride[]`                      | —            | See "Overrides" below.                                                                                                                                                  |
| `strokeWidthOverrides` | `RlottieStrokeWidthOverride[]`                  | —            | See "Overrides" below.                                                                                                                                                  |
| `metricsEnabled`       | `boolean`                                       | `false`      | Gates BOTH native metrics collection and the `onMetrics` event — disabled means collection itself is off, not merely unreported.                                        |

Playback-shaping props (`loop`, `repeatCount`, `speed`, `startFrame`,
`endFrame`, `autoPlay`) are coalesced into a single native `configure(...)`
call per prop transaction, never one native call per setter — so a
partially-applied config is never briefly live.

### Overrides

Three parallel, narrow arrays rather than one polymorphic/discriminated-union
array — simpler to validate identically on both platforms.

```ts
interface RlottieColorOverride {
  keyPath: string; // rlottie key path, e.g. "layer.group.fill"
  color: string; // "#RRGGBB" or "#AARRGGBB"
}
interface RlottieOpacityOverride {
  keyPath: string;
  opacity: number; // 0..1, clamped natively (not rejected) if out of range
}
interface RlottieStrokeWidthOverride {
  keyPath: string;
  width: number; // pixels, > 0, clamped natively if out of range
}
```

Known limitations of overrides, at the point where they'll actually bite:

- **`colorOverrides` discards alpha on both platforms.** The alpha channel of
  `#AARRGGBB` is parsed but dropped — `RenderCoordinator::setColor` has no
  alpha parameter.
- **`opacityOverrides` uses rlottie's `FillOpacity`.** rlottie has no single
  combined fill+stroke opacity property, so **a stroke-only shape's opacity
  override has no visible effect** (the same fill-only scope `colorOverrides`
  already has).
- **An override applied after load may not affect a layer with no animated
  keyframes.** This vendored rlottie skips re-evaluating a layer's paint on
  repeat renders when the layer has no animated keyframes, so `setValue()`
  (and by extension any of these three override setters) added after the
  animation is already loaded may not take effect on such a layer. This is
  pre-existing rlottie engine behavior, not specific to this library — it
  equally affects `colorOverrides`.
- All three apply on the same serialized render worker as rendering (never
  from the calling thread directly), so they never race a frame being
  rendered — but that also means they are not synchronous: the effect appears
  on a subsequent frame, not immediately on return.

## Events

All six are direct RN events (`NativeSyntheticEvent<T>`); `event.nativeEvent`
holds the payload below. **No event fires after the view is unmounted.**

| event               | payload               | fires                                                   |
| ------------------- | --------------------- | ------------------------------------------------------- |
| `onAnimationLoaded` | `RlottieLoadedEvent`  | Once the source is parsed and ready.                    |
| `onAnimationError`  | `RlottieErrorEvent`   | On any failure — see "Error codes" below.               |
| `onAnimationStart`  | `{}`                  | Playback starts (including resume from `stop`/`reset`). |
| `onAnimationPause`  | `{}`                  | Playback pauses.                                        |
| `onAnimationLoop`   | `{}`                  | A loop iteration completes and another begins.          |
| `onAnimationFinish` | `{}`                  | Playback completes and will not loop again.             |
| `onMetrics`         | `RlottieMetricsEvent` | Opt-in, throttled — see below.                          |

```ts
interface RlottieLoadedEvent {
  width: number; // intrinsic composition size, px, as authored
  height: number;
  duration: number; // seconds
  frameRate: number;
  totalFrames: number;
  markers: RlottieMarker[];
}
interface RlottieMarker {
  name: string;
  startFrame: number;
  endFrame: number;
}
interface RlottieErrorEvent {
  code: RlottieErrorCode;
  message: string; // developer-facing detail, not localized, don't show to end users
}
```

There is **no `onFrame` event**, by design — per-frame data never crosses the
bridge. `onMetrics` is the sole recurring event and exists specifically so it
doesn't become one:

- Only emitted while `metricsEnabled` is `true`, and throttled to **at most
  once per second**.
- Is **not** a proxy for per-frame telemetry; it reports aggregates over a
  bounded rolling window (~512 renders), not the whole session's history.

```ts
interface RlottieMetricsEvent {
  parseMs: number; // wall time of the last successful parse (load)
  firstFrameMs: number; // source set → first frame published
  renderP50Ms: number; // render-duration percentiles, bounded rolling window
  renderP95Ms: number;
  renderP99Ms: number;
  framesRendered: number;
  framesDropped: number; // coalesced by latest-frame-wins — NOT an error, see below
  bufferAllocCount: number; // should stop increasing at steady state
  peakBufferBytes: number; // THIS view's buffers, not process RSS — see below
  uiStallCount: number; // display ticks that arrived later than expected
  uiStallMaxMs: number;
}
```

Two things worth internalizing before you alarm on these fields:

- **`framesDropped` rising under load is the latest-frame-wins policy working
  correctly, not a bug.** The library keeps at most one render in flight per
  view and coalesces away stale requests rather than queuing them; a rising
  count means it's discarding backlog instead of falling further behind.
- **`peakBufferBytes` is this view's own frame buffers, not total process
  memory.** There is no portable way to read process RSS from inside a
  library, so this intentionally reports a narrower, honest number rather than
  something that looks like total memory use but isn't.

**Platform divergence in the wire format** (not just semantics): Android
writes the four integer fields (`framesRendered`, `framesDropped`,
`bufferAllocCount`, `peakBufferBytes`) via `WritableMap.putInt` — a 32-bit,
**saturating** write. iOS boxes the native `uint64_t` exactly. This is
unreachable at the default `maxPixels` (peak ~134 MB), but becomes reachable
if you raise `InputLimits.maxPixels` via `configure()` far enough to push
`peakBufferBytes` past 2^31. If you rely on these counters for anything beyond
rough monitoring, treat Android's four integer fields as saturating at
`2147483647`.

## Error codes (`RlottieErrorCode`)

Passed through **verbatim** from `cpp/ErrorCode.h`, the single source of truth
both native adapters map through — never re-mapped, re-cased, or filtered at
the bridge layer.

| code                 | typically means                                                                                                                                                                               |
| -------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `INVALID_SOURCE`     | The `source` shape wasn't recognized, used a disallowed URI scheme (e.g. bare `http://`/unopted `https://`), or (native-side) a `{path}`/`resourcePath` resolved outside app-private storage. |
| `SOURCE_NOT_FOUND`   | A file-backed source (`path`/`uri`) pointed at a file that doesn't exist.                                                                                                                     |
| `SOURCE_TOO_LARGE`   | The JSON exceeded `InputLimits.maxJsonBytes` (16 MiB default), or the animation declares more frames than `InputLimits.maxFrames` (100,000 default).                                          |
| `PARSE_FAILED`       | The JSON failed to parse — including JSON nested deeper than `InputLimits.maxJsonDepth` (512 default; see the note below), which is rejected before rlottie's parser ever sees it.            |
| `INVALID_DIMENSIONS` | Zero width/height, a dimension outside 32-bit range, or a pixel count (width × height) exceeding `InputLimits.maxPixels` (4096×4096 default).                                                 |
| `ALLOCATION_FAILED`  | A frame-buffer or animation-model allocation failed (including a request too large for `std::vector` to represent, mapped here rather than crashing).                                         |
| `RENDER_FAILED`      | rlottie's render call itself failed.                                                                                                                                                          |
| `RELEASED`           | An operation was attempted on a player/view whose native handle has already been torn down.                                                                                                   |

`NONE` is the core's internal "no error" sentinel and is never delivered as an
`onAnimationError` payload.

**Why `maxJsonDepth` exists as a crash guard, not a policy knob**: rlottie's
vendored rapidjson parses with recursive descent, so a deeply nested (but
otherwise small) JSON payload can exhaust the native stack and crash the
process — a `SIGSEGV`, not a catchable error. A byte-size limit alone does not
bound nesting depth (a nesting token is as small as one byte, `[`), which is
why depth is checked independently before rlottie ever parses the data. This
guard only covers the in-memory `{json}` path — `loadFromFile` hands a path
straight to rlottie's own file reader, so it isn't covered by this
JS-observable check (see `docs/troubleshooting.md`).

## Source forms

The C++ core never sees a URI and never performs network I/O; the platform
resolvers (`RNRlottieSourceResolver` / `RlottieSourceResolver.kt`) turn a
`source` prop into one of two shapes the core actually accepts
(`{json, cacheKey, resourcePath?}` or `{path, cacheKey}`), and `src/source.ts`
does the JS-side normalization before the value ever reaches native.

```ts
type RlottieSource =
  | number // require()d asset id
  | {json: string | RlottieJsonObject; cacheKey?: string; resourcePath?: string}
  | {uri: string; cacheKey?: string} // file://, asset:///, content://
  | {path: string; cacheKey?: string}; // pre-resolved absolute path
```

| form                                        | resolves to                                       | notes                                                                                                                                                                                                                                                                                                  |
| ------------------------------------------- | ------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `require('./anim.json')`                    | RN asset registry → a `file://`/bundled uri       | Note: `require('./anim.json')` is inlined by Metro as a **plain object**, not an asset id, so it actually takes the object `json` path below, not this one. This form is for assets registered through the asset registry (e.g. non-JSON asset types).                                                 |
| `{json: '<raw json string>'}`               | `setSourceData`                                   | Stringified/hashed on the JS thread.                                                                                                                                                                                                                                                                   |
| `{json: {...}}` (parsed object)             | `setSourceData`                                   | `JSON.stringify`'d **exactly once**, memoized on the object's identity in a `WeakMap`. Passing a fresh object literal every render defeats the memoization and re-stringifies + re-hashes a potentially multi-megabyte payload every render — hoist the object to module scope or memoize it yourself. |
| `{uri: 'file:///…'}`                        | `setSourceFile` after canonicalization            |                                                                                                                                                                                                                                                                                                        |
| `{uri: 'asset:///relative/path.json'}`      | bundled asset → `setSourceFile`                   | **Same scheme, `asset:///`, on BOTH iOS and Android** — resolves against Android's `assets/` (via `AssetManager`, copied into app-private storage) and iOS's main bundle path respectively. Use three slashes (empty authority).                                                                       |
| `{uri: 'content://…'}` (Android only)       | copied into app-private storage → `setSourceFile` |                                                                                                                                                                                                                                                                                                        |
| `{path: '/absolute/app/private/path.json'}` | `setSourceFile`                                   | Native canonicalizes and **confines it to app-private storage** (`filesDir`/`cacheDir`/`noBackupFilesDir` on Android; the app sandbox on iOS) — anything that resolves outside it, including via `..` or a symlink, is `INVALID_SOURCE`, not silently clamped.                                         |
| `{uri: 'http://…'}`                         | rejected                                          | Always `INVALID_SOURCE` — plain HTTP is never allowed, `allowRemoteSources` cannot opt out of this.                                                                                                                                                                                                    |
| `{uri: 'https://…'}`                        | rejected in v1                                    | `INVALID_SOURCE` whether or not `allowRemoteSources` is set — the message differs (see below) but the outcome doesn't. Remote fetch is v1.1 scope; the native layer never performs network I/O in v1.                                                                                                  |

Limits enforced natively (`cpp/InputLimits.h`, overridable via
`Rlottie.configure()` — see below): `maxJsonBytes` (16 MiB), `maxPixels`
(4096×4096), `maxFrames` (100,000), `maxJsonDepth` (512 nesting levels),
`maxExternalAssets` (64) and `maxExternalBytes` (32 MiB) for `resourcePath`'s
external assets (enforced by the resolvers over the asset directory, not by
the core, which has no pre-parse API to enumerate embedded assets).

### `allowRemoteSources`

`false` by default. In v1 this prop **only changes the wording of the
rejection message** for an `https://` `source.uri` — it does not make remote
loading work. An `http://` uri is rejected unconditionally regardless of this
flag (HTTPS-only is non-negotiable for any future remote loading). Both native
resolvers independently reject `http(s)://` too, as defense in depth — the JS
gate is not the only check.

### Cache keys

`cacheKey` composes as `sha256(contentOrLocation):callerCacheKey` on the JS
side, and native appends `:rlottieCommit:parseConfig` before it reaches
rlottie's model cache — so a caller-supplied key alone can never alias two
different payloads for `{json}` sources.

**Known weakness for file-backed sources (`{uri}`/`{path}`): the key is
location-derived, not content-derived.** Two different files written
successively to the same path produce the _same_ cache key unless the caller
also changes `cacheKey` or the path itself — JS has no cheap way to detect
that file content changed underneath a stable path, since hashing the bytes
would mean reading the file synchronously on the JS thread. If you overwrite a
file at a stable path, pass a new `cacheKey` (or a new path) so the change is
picked up.

## Imperative ref (`RlottieViewRef`)

```ts
interface RlottieViewRef {
  play(options?: RlottiePlayOptions): void;
  pause(): void;
  resume(): void;
  stop(): void;
  reset(): void;
  seekToProgress(progress: number): void; // clamped to 0..1 natively
  seekToFrame(frame: number): void;
  setSpeed(speed: number): void;
}

interface RlottiePlayOptions {
  startFrame?: number;
  endFrame?: number;
  marker?: string;
}
```

- Obtained the normal way: `const ref = useRef<RlottieViewRef>(null)`, passed
  to `<RlottieView ref={ref} />`.
- Every method is a **fire-and-forget command dispatch**; none return a
  promise and none throw. A call before the native view has mounted (e.g. in
  an effect racing mount) is silently dropped, not queued and not an error.
- `play({startFrame, endFrame})` is a **one-off override for that call only**
  — it does not change the view's configured `startFrame`/`endFrame` props, so
  a later argument-less `play()` does not inherit it.
- `play({marker: 'name'})` **wins outright** over `startFrame`/`endFrame` when
  both are provided in the same call — they are not merged. `marker` refers to
  a named segment from `RlottieLoadedEvent.markers`; an unknown marker name is
  a **silent no-op**, not an `onAnimationError` (a missing marker is treated as
  a caller mistake, not an animation failure).

**`dispatchRlottieCommand` (from `src/commands.ts`) is deliberately NOT
exported.** It's the one place `UIManager.dispatchViewManagerCommand` and
`findNodeHandle` are allowed to appear, and it resolves the command id
dynamically via `UIManager.getViewManagerConfig('RlottieView').Commands` (with
a same-behavior string-name fallback) rather than a hardcoded integer — this
sidesteps an iOS legacy-bridge trap where an out-of-range integer command id
raises `NSRangeException` and crashes the app outright, and it also carries
the unmounted-view no-op guard. Hand-rolling your own
`dispatchViewManagerCommand` call would bypass both. `RlottieCommand` (the
numeric id enum) is exported for reference only — e.g. for debugging or a
custom native integration you write yourself — not as something to dispatch
with directly.

## `Rlottie` (the global module)

Process-global operations only — never per-view playback. All three methods
are **Promise-based on both platforms**.

```ts
const Rlottie: {
  configure(options: RlottieConfigureOptions): Promise<void>;
  clearModelCache(): Promise<void>;
  getNativeVersion(): Promise<RlottieNativeVersion>;
  isAvailable(): boolean;
};
// also exported individually: configure, clearModelCache, getNativeVersion, isAvailable
```

| method                         | behavior                                                                                                                                                                                                    |
| ------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `configure({modelCacheSize?})` | Sets how many _parsed_ animation models rlottie keeps cached process-wide. A missing/negative `modelCacheSize` is a no-op (not clamped to 0) — confirm what actually took effect with `getNativeVersion()`. |
| `clearModelCache()`            | Flushes the model cache, preserving the configured size.                                                                                                                                                    |
| `getNativeVersion()`           | Returns `{rlottieCommit: string, modelCacheSize: number}` — the pinned rlottie commit and the cache size currently in effect. Useful in bug reports, since the rlottie revision affects rendering behavior. |
| `isAvailable()`                | Synchronous. `true` when the native module is linked. Lets a consumer degrade gracefully instead of hitting the linking error.                                                                              |

The native module is resolved **lazily, per call** — importing
`react-native-rlottie` never throws even if no native runtime is present (a
test file, Storybook, a web build). Calling any of these three when the native
module isn't linked throws a diagnostic `Error` explaining the likely cause
(missing `pod install`, missing rebuild, disabled autolinking) — see
`docs/troubleshooting.md`.
