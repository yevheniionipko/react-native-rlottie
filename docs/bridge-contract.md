# The frozen bridge contract (Chunks 2.3 + 3.3)

This is the **single source of truth** for the prop, event, and command surface
the legacy bridge exposes. iOS (`ios/RNRlottieViewManager.mm`) and Android
(`android/src/main/java/com/rlottie/RlottieViewManager.kt`) must match it
**byte-for-byte** — same names, same numeric ids, same payload keys, same types.
Phase 4's TypeScript API (Chunk 4.1) is written against this document, and
divergence between the platforms shows up there as a runtime bug on exactly one
of them, which is the failure mode this file exists to prevent.

Derived from plan §3 (props/ref commands) and §11 (commands/events).

## Native component name

`RlottieView` — the string `requireNativeComponent<...>('RlottieView')` resolves.
iOS: `RCT_EXPORT_MODULE()` on `RNRlottieViewManager` exports `RNRlottieView`,
so the manager **must** use `RCT_EXPORT_MODULE(RlottieView)` explicitly.
Android: `SimpleViewManager.getName()` returns `"RlottieView"`.

## Commands — stable numeric ids

Never renumber these; they are persisted in released JS bundles.

| id | name | args |
|----|------|------|
| 1 | `play` | `[startFrame: int \| -1, endFrame: int \| -1]` — `-1` means "unset" |
| 2 | `pause` | none |
| 3 | `resume` | none |
| 4 | `stop` | none |
| 5 | `reset` | none |
| 6 | `seekToProgress` | `[progress: double]` (0..1) |
| 7 | `seekToFrame` | `[frame: int]` |
| 8 | `setSpeed` | `[speed: double]` |

Both platforms **must** accept the command as an integer id *and* as the string
name. RN changed `dispatchViewManagerCommand` from int to string ids across
versions, and this library supports a range of them (plan §10) — so Android's
`receiveCommand` must be overridden for both `Int` and `String`, and
`getCommandsMap()` must still be provided for the integer path.

Commands that arrive before the view has a live native handle are dropped
silently, not queued and not an error.

`play`'s frame range is a **one-off override for that play only** — it must not
be written into the persistent playback config, or a later argument-less `play()`
would inherit it. Both platforms pass the values straight to
`PlaybackController::play(optional, optional)` (iOS `-playFromFrame:toFrame:`,
Android `nativePlay(h, start, end)`); `-1` becomes `std::nullopt`. Note the
sentinel check cannot be `> 0`: frame 0 is legitimate and `std::size_t` cannot
hold `-1`.

### iOS numeric-id fragility (verified against RN 0.77.3)

`-[RCTUIManager dispatchViewManagerCommand:...]` resolves an integer id as
`moduleData.methods[commandID]` — a **raw array index** into the manager's
`RCT_EXPORT_METHOD`s in declaration order (`RCTUIManager.mm`, and
`RCTModuleData.mm -calculateMethods`, which appends the manager's own methods
before any inherited ones). `RCTComponentData` then publishes `Commands` to JS as
those same sequential indices. Consequences for `ios/RNRlottieViewManager.mm`:

- Declaration index 0 is a reserved no-op (`__reservedCommandSlot0`) purely so
  that index N == command id N, matching Android's explicit `getCommandsMap()`.
- That file must export **only** these 9 methods, in **exactly** this order. An
  extra or reordered `RCT_EXPORT_METHOD` silently shifts every later id.
- An integer id outside 0…8 raises `NSRangeException` rather than failing
  gracefully, so the JS layer must never emit one.

The string-name path has none of this fragility, so Chunk 4.1 should prefer
`UIManager.getViewManagerConfig('RlottieView').Commands.<name>` over a hardcoded
integer.

## Events — direct events

| event | payload |
|-------|---------|
| `onAnimationLoaded` | `{width: int, height: int, duration: double, frameRate: double, totalFrames: int, markers: [{name: string, startFrame: int, endFrame: int}]}` |
| `onAnimationError` | `{code: string, message: string}` |
| `onAnimationStart` | `{}` |
| `onAnimationPause` | `{}` |
| `onAnimationLoop` | `{}` |
| `onAnimationFinish` | `{}` |

There is **no `onFrame` event**, by design (plan §11) — per-frame bridge traffic
is the exact problem this library exists to avoid. Do not add one.

`code` is passed through **verbatim** from the native layer, which gets it from
`cpp/ErrorCode.h` — the one function both adapters map through. Do not re-map,
re-case, or filter codes at the bridge layer.

> Known gap for Chunk 4.1: `cpp/ErrorCode.h` emits `INVALID_DIMENSIONS` and
> `RELEASED`, which plan §11's TS union does not list (it lists
> `UNSUPPORTED_FEATURE`, which nothing emits). The TS union must be reconciled
> with `ErrorCode.h`, not the other way round.

No event may fire after the view is unmounted / dropped.

## Props

| prop | type | notes |
|------|------|-------|
| `source` | object | `{json?: string, path?: string, cacheKey?: string, resourcePath?: string}` after resolution. Full resolution of bundled/asset/content URIs is Chunk 2.4/3.4 — see below. |
| `autoPlay` | bool | default `false` |
| `loop` | bool | default `false` |
| `repeatCount` | int | `0` = infinite when `loop` |
| `speed` | double | default `1.0`; negative plays in reverse |
| `progress` | double | 0..1, seeks |
| `startFrame` | int | default `0` |
| `endFrame` | int | `0` = last frame |
| `resizeMode` | string | `contain` \| `cover` \| `stretch` \| `center`; default `contain`. **iOS only so far** — maps to `layer.contentsGravity`; Android's `onDraw` still always stretches (accepted + validated, but a no-op). |
| `renderScale` | double | default `1.0` |
| `pauseWhenInactive` | bool | default `true` |
| `cacheStrategy` | string | `none` \| `model`; default `model`. **iOS only so far** — plumbed into `useModelCache`; Android's source natives take no cache flag yet (Chunk 3.4). |
| `colorOverrides` | array | `[{keyPath: string, color: string}]`, `color` as `#RRGGBB`/`#AARRGGBB`. Alpha is parsed but **discarded on both platforms** — `RenderCoordinator::setColor` has no alpha parameter. |

Playback-shaping props (`loop`, `repeatCount`, `speed`, `startFrame`, `endFrame`,
`autoPlay`) map onto the single native `configure(...)` call, so they must be
**coalesced and applied once per prop transaction**, not one native call per
setter — otherwise a partially-applied config is briefly live.

## Source resolution boundary

The C++ core never sees a URI and never performs I/O over the network (plan §12
/ §16). At this layer, accept only an already-resolved shape:

- `{json: "..."}` → `setSourceData(json, cacheKey, resourcePath)`
- `{path: "/abs/app/private/path.json"}` → `setSourceFile(path)`

Anything else (a `uri`, an `http(s)` URL, a require()'d asset id) is **out of
scope for 2.3/3.3** — leave a clearly marked seam for `RlottieSourceResolver`
(Chunk 3.4 / 2.4) and emit an `onAnimationError` with code `INVALID_SOURCE`
rather than attempting resolution here.
