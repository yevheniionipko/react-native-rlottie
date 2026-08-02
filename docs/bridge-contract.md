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

| id  | name             | args                                                                |
| --- | ---------------- | ------------------------------------------------------------------- |
| 1   | `play`           | `[startFrame: int \| -1, endFrame: int \| -1]` — `-1` means "unset" |
| 2   | `pause`          | none                                                                |
| 3   | `resume`         | none                                                                |
| 4   | `stop`           | none                                                                |
| 5   | `reset`          | none                                                                |
| 6   | `seekToProgress` | `[progress: double]` (0..1)                                         |
| 7   | `seekToFrame`    | `[frame: int]`                                                      |
| 8   | `setSpeed`       | `[speed: double]`                                                   |
| 9   | `playMarker`     | `[name: string]` — added in Phase 6; see "Phase 6 additions"        |

Both platforms **must** accept the command as an integer id _and_ as the string
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
- That file must export **only** the commands in the table above plus the
  reserved slot — currently **10** methods (slot 0 + ids 1–9) — in **exactly**
  that order. An extra or reordered `RCT_EXPORT_METHOD` silently shifts every
  later id.
- New commands are **append-only**: declare them after the current last method,
  never inserted. `playMarker` (id 9) was the first such addition.
- An integer id outside 0…9 raises `NSRangeException` rather than failing
  gracefully, so the JS layer must never emit one.

The string-name path has none of this fragility, so Chunk 4.1 should prefer
`UIManager.getViewManagerConfig('RlottieView').Commands.<name>` over a hardcoded
integer.

## Events — direct events

| event               | payload                                                                                                                                       |
| ------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| `onAnimationLoaded` | `{width: int, height: int, duration: double, frameRate: double, totalFrames: int, markers: [{name: string, startFrame: int, endFrame: int}]}` |
| `onAnimationError`  | `{code: string, message: string}`                                                                                                             |
| `onAnimationStart`  | `{}`                                                                                                                                          |
| `onAnimationPause`  | `{}`                                                                                                                                          |
| `onAnimationLoop`   | `{}`                                                                                                                                          |
| `onAnimationFinish` | `{}`                                                                                                                                          |

There is **no `onFrame` event**, by design (plan §11) — per-frame bridge traffic
is the exact problem this library exists to avoid. Do not add one.

`code` is passed through **verbatim** from the native layer, which gets it from
`cpp/ErrorCode.h` — the one function both adapters map through. Do not re-map,
re-case, or filter codes at the bridge layer.

> RESOLVED in Chunk 4.1. `cpp/ErrorCode.h` emits `INVALID_DIMENSIONS` and
> `RELEASED`, which plan §11's draft TS union did not list, and lists
> `UNSUPPORTED_FEATURE`, which nothing emits. `RlottieErrorCode` in
> `src/types.ts` is now reconciled with `ErrorCode.h`: those two added,
> `UNSUPPORTED_FEATURE` dropped. `NONE` stays out — it is the core's internal
> "no error" sentinel and never reaches an error event. `ErrorCode.h` remains
> the source of truth; update it first, then the TS union.

No event may fire after the view is unmounted / dropped.

## Props

| prop                 | type   | notes                                                                                                                                                                                                        |
| -------------------- | ------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `source`             | object | `{json?: string, path?: string, cacheKey?: string, resourcePath?: string}` after resolution. Full resolution of bundled/asset/content URIs is Chunk 2.4/3.4 — see below.                                     |
| `autoPlay`           | bool   | default `false`                                                                                                                                                                                              |
| `loop`               | bool   | default `false`                                                                                                                                                                                              |
| `repeatCount`        | int    | `0` = infinite when `loop`                                                                                                                                                                                   |
| `speed`              | double | default `1.0`; negative plays in reverse                                                                                                                                                                     |
| `progress`           | double | 0..1, seeks                                                                                                                                                                                                  |
| `startFrame`         | int    | default `0`                                                                                                                                                                                                  |
| `endFrame`           | int    | `0` = last frame                                                                                                                                                                                             |
| `resizeMode`         | string | `contain` \| `cover` \| `stretch` \| `center`; default `contain`. **iOS only so far** — maps to `layer.contentsGravity`; Android's `onDraw` still always stretches (accepted + validated, but a no-op).      |
| `renderScale`        | double | default `1.0`                                                                                                                                                                                                |
| `pauseWhenInactive`  | bool   | default `true`                                                                                                                                                                                               |
| `cacheStrategy`      | string | `none` \| `model`; default `model`. **iOS only so far** — plumbed into `useModelCache`; Android's source natives take no cache flag yet (Chunk 3.4).                                                         |
| `colorOverrides`     | array  | `[{keyPath: string, color: string}]`, `color` as `#RRGGBB`/`#AARRGGBB`. Alpha is parsed but **discarded on both platforms** — `RenderCoordinator::setColor` has no alpha parameter.                          |
| `allowRemoteSources` | bool   | default `false`. **JS-only gate (Chunk 5.4); never crosses the bridge.** `RlottieView` consumes it in `normalizeSource` and does not forward it in `RlottieNativeProps` — see "Remote loading policy" below. |

Playback-shaping props (`loop`, `repeatCount`, `speed`, `startFrame`, `endFrame`,
`autoPlay`) map onto the single native `configure(...)` call, so they must be
**coalesced and applied once per prop transaction**, not one native call per
setter — otherwise a partially-applied config is briefly live.

## Phase 6 additions — dynamic properties

Frozen before implementation, exactly like the Phase 2/3 surface above: three
streams implement against this section concurrently, so anything ambiguous here
becomes a platform divergence.

### Command id 9 — `playMarker`

| id  | name         | args             |
| --- | ------------ | ---------------- |
| 9   | `playMarker` | `[name: string]` |

Plays the frame segment of the named marker (markers come from the Lottie source
and already reach JS in `onAnimationLoaded`). Like `play`'s frame range, this is
a **one-off** segment for this playback — it must not be written into the
persistent config.

**iOS: `playMarker` MUST be declared LAST** in `ios/RNRlottieViewManager.mm`,
after `setSpeed`, so it lands at declaration index 9. Inserting it anywhere
earlier silently renumbers every command after it — see "iOS numeric-id
fragility". This is the first command added since the ids were frozen, and the
append-only rule is exactly why they were frozen.

An unknown marker name is a **silent no-op**, consistent with how a command to an
unmounted view is dropped. It does NOT emit `onAnimationError`: that would mean a
new error code, and a missing marker is a caller mistake rather than a failure of
the animation. Native returns a bool the adapters ignore.

TS surfaces this through the existing ref method rather than a new one:
`RlottiePlayOptions` gains `marker?: string`. When `marker` is set it wins and
`startFrame`/`endFrame` are ignored (documented, not silently merged) — mixing a
named segment with an explicit frame range has no coherent meaning.

### Props — opacity and stroke overrides

Parallel in shape to the existing `colorOverrides`, deliberately NOT merged into
one polymorphic array: three narrow typed arrays are simpler to validate on both
platforms than one discriminated union crossing the bridge.

| prop                   | type  | notes                                                     |
| ---------------------- | ----- | --------------------------------------------------------- |
| `opacityOverrides`     | array | `[{keyPath: string, opacity: number}]`, `opacity` is 0..1 |
| `strokeWidthOverrides` | array | `[{keyPath: string, width: number}]`, `width` in px, > 0  |

`keyPath` is an rlottie key path, same syntax as `colorOverrides`. Out-of-range
values are clamped natively rather than rejected — a bad override should not fail
the whole animation.

All three override props apply **on the same serialized render worker** as
rendering (plan §6). They must never mutate the animation while a frame is being
rendered; route them through `RenderCoordinator`, never directly at the core.

### Core API (what the platform layers bind to)

```cpp
// RlottiePlayerCore — [worker] only
void setColor(const std::string& keyPath, float r, float g, float b);   // exists
void setOpacity(const std::string& keyPath, float opacity);             // 0..1
void setStrokeWidth(const std::string& keyPath, float width);

// RenderCoordinator — thread-safe, hops onto the worker
void setColor(std::string keyPath, float r, float g, float b);          // exists
void setOpacity(std::string keyPath, float opacity);
void setStrokeWidth(std::string keyPath, float width);

// PlaybackController — [UI] only. False when the marker is unknown.
bool playMarker(const std::string& name);
```

## Source resolution boundary

The C++ core never sees a URI and never performs I/O over the network (plan §12
/ §16). The native layer accepts only an already-resolved shape:

- `{json: "..."}` → `setSourceData(json, cacheKey, resourcePath)`
- `{path: "/abs/app/private/path.json"}` → `setSourceFile(path)`

Chunks 2.3/3.3 implement only those two and emit `onAnimationError` /
`INVALID_SOURCE` for anything else. **Chunks 2.4/3.4 own the resolver** that
turns a JS source into one of them.

### Resolver contract (Chunks 2.4 / 3.4)

Both platforms must accept the same source kinds and reject the same ones. Plan
§10's file list names `RlottieSourceResolver.kt` for Android and no iOS
counterpart; that asymmetry is not intentional — iOS gets
`ios/RNRlottieSourceResolver.h/.mm` so behaviour stays identical.

v1 accepts, per plan §3:

| input                                   | resolves to                                     |
| --------------------------------------- | ----------------------------------------------- |
| `{json: "<raw json string>", cacheKey}` | `setSourceData`                                 |
| `{uri: "file:///…"}`                    | `setSourceFile` after canonicalization          |
| `{uri: "content://…"}` (Android)        | copy into app-private storage → `setSourceFile` |
| `{uri: "asset:///<relative/path>"}`     | bundled asset → `setSourceFile`                 |

**The bundled-asset scheme is `asset://` on BOTH platforms.** It resolves
against Android's `assets/` root (via `AssetManager`, copied into app-private
storage) and against `[[NSBundle mainBundle] bundlePath]` on iOS. The roots
necessarily differ; the scheme deliberately does not, so a single JS `source`
value works unchanged cross-platform. Use three slashes (`asset:///foo.json`) —
an empty authority — matching RN's own convention for bundled non-image assets.
This was left unspecified in the first draft and each platform independently
invented a different scheme (`asset://` vs `bundle://`); they are now unified,
and neither an alias nor a platform-conditional form should be reintroduced.

v1 **rejects** with `INVALID_SOURCE` (v1.1 territory, plan §3): `http(s)://`
remote URIs, `.lottie` containers, and any scheme not on the allow-list. Never
silently fall back to a different source kind.

### Remote loading policy (Chunk 5.4 — the gate, not the download)

Plan §16 wants remote loading opt-in via `<RlottieView allowRemoteSources
source={{uri: 'https://...'}} />`. v1 stubs the gate; v1.1 does the actual
fetch. Concretely, in v1:

- `allowRemoteSources` is a **JS-only prop**. `src/source.ts`'s
  `normalizeSource(source, allowRemoteSources)` uses it to pick which
  rejection message to return; it never reaches `RlottieNativeProps` or either
  native adapter (`src/RlottieNativeComponent.ts` deliberately omits it).
- An `https://` `source.uri` is `INVALID_SOURCE` **either way** — the flag
  changes wording, not outcome. Without the flag, the message tells the
  developer the flag exists and what it will eventually do. With the flag set,
  the message says remote loading is genuinely not implemented yet in this
  version, rather than implying the URL is malformed.
- `http://` (no TLS) is **always** `INVALID_SOURCE`, flag or no flag — plan
  §16 mandates HTTPS-only for any future remote loading, and that is not
  something `allowRemoteSources` can opt out of.
- **The TS gate is defence in depth, not the only check.** Both native
  resolvers (`RNRlottieSourceResolver` / `RlottieSourceResolver.kt`) must
  independently reject `http(s)://` regardless of what crosses the bridge —
  they cannot assume a well-behaved JS layer sent them, since a native
  adapter can in principle be driven by anything holding a view handle. When
  the real fetch path lands in v1.1, this row must be revisited; until then
  neither the C++ core nor either native adapter performs network I/O
  (plan §12/§16).

Security rules (plan §16), binding on both resolvers:

- Allow-list URI schemes; reject anything else rather than "best effort".
- Canonicalize every path and **reject filesystem escape** — a `..` sequence, a
  symlink out of the sandbox, or any resolved path outside the app-private
  directory is `INVALID_SOURCE`, not a clamp.
- Enforce the byte limit from `cpp/InputLimits.h` (`maxJsonBytes`, 16 MiB)
  **before** reading a file fully into memory; oversize is `SOURCE_TOO_LARGE`.
- A missing file is `SOURCE_NOT_FOUND`.
- Never log raw animation JSON.
- `resourcePath` (rlottie's external-asset root) must be a validated
  app-private directory or empty — never a caller-supplied arbitrary root.

Cache keys follow plan §15: derive from source identity **and** content, not a
caller-supplied string alone, so two different payloads cannot silently share a
key. The full `sha256(json) + ":" + callerCacheKey` scheme is Chunk 5.3; until
then, include `kRlottieVersion`'s commit and do not trust a bare caller key.

## Global module (Chunks 2.4 / 3.4)

Native module name `RlottieModule`, exposing **only** process-global operations
(plan §15) — never per-view playback:

| method                        | behaviour                                                |
| ----------------------------- | -------------------------------------------------------- |
| `configure({modelCacheSize})` | → `ModelCacheController::setModelCacheSize`              |
| `clearModelCache()`           | → `ModelCacheController::clearModelCache`                |
| `getNativeVersion()`          | → `{rlottieCommit: kRlottieCommit, modelCacheSize: int}` |

**All three are Promise-based on both platforms** — the TS wrapper `await`s them.
Do not make any of them fire-and-forget on one platform only: that would give
`await Rlottie.configure(...)` a platform-conditional error contract.
