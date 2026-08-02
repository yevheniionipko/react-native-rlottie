// Chunk 4.1 — the public TypeScript surface.
//
// These types are the JS-side half of `docs/bridge-contract.md`. That document
// is the authority: prop names, event names, and payload keys here must match
// what `ios/RNRlottieViewManager.mm` and
// `android/src/main/java/com/rlottie/RlottieViewManager.kt` actually export. A
// mismatch is not a compile error anywhere — it is a silently-undefined prop at
// runtime on one platform — so change all three together.

import type {NativeSyntheticEvent, ViewProps} from 'react-native';

// --- Sources ----------------------------------------------------------------

/**
 * A parsed Lottie animation as a plain object — what `require('./anim.json')`
 * evaluates to, since Metro inlines JSON imports as objects rather than as
 * asset ids.
 *
 * It is stringified EXACTLY ONCE, in `normalizeSource` (plan §12), and the
 * result is memoized on the object's identity. Passing a fresh object literal
 * on every render defeats that memoization and re-stringifies + re-hashes a
 * potentially multi-megabyte payload on the JS thread each time — hoist the
 * object to module scope or memoize it.
 */
export type RlottieJsonObject = Record<string, unknown>;

/**
 * Accepted `source` shapes. All resolution to an app-controlled path or JSON
 * string happens natively (`RNRlottieSourceResolver` / `RlottieSourceResolver`);
 * the C++ core never sees a URI and never performs network I/O (plan §12/§16).
 *
 * Bundled assets use `asset:///<relative/path>` on BOTH platforms — Android's
 * `assets/` root and the iOS main bundle. See docs/bridge-contract.md.
 *
 * `http(s)://` is rejected in v1 (it is v1.1 scope, plan §3, gated behind
 * `allowRemoteSources` in Chunk 5.4). Rejection is explicit — there is no
 * silent fallback to another source kind.
 */
export type RlottieSource =
  /** A `require()`d asset id, resolved through RN's asset registry. */
  | number
  /** Raw JSON, either already stringified or as a parsed object. */
  | {json: string | RlottieJsonObject; cacheKey?: string; resourcePath?: string}
  /** `file://`, `asset://`, or (Android) `content://`. */
  | {uri: string; cacheKey?: string}
  /** An already-resolved absolute path, for callers that resolve ahead of time. */
  | {path: string; cacheKey?: string};

/**
 * What actually crosses the bridge as the `source` prop, after normalization.
 * Exactly one of `json` / `uri` / `path` is present — the native resolvers
 * branch on that.
 */
export type NormalizedRlottieSource =
  | {json: string; cacheKey: string; resourcePath?: string}
  | {uri: string; cacheKey: string}
  | {path: string; cacheKey: string};

// --- Errors -----------------------------------------------------------------

/**
 * Machine-readable error codes, emitted by `onAnimationError`.
 *
 * RECONCILED WITH `cpp/ErrorCode.h`, which is the single source of truth both
 * native adapters map through. This deliberately differs from plan §11's draft
 * union in two ways:
 *
 *   * `INVALID_DIMENSIONS` and `RELEASED` are ADDED — `errorCodeString()` emits
 *     both, so omitting them would leave real errors untypeable.
 *   * `UNSUPPORTED_FEATURE` is REMOVED — nothing in the native layer ever emits
 *     it, so it was dead surface that implied a capability check we don't do.
 *
 * `NONE` is intentionally absent: it is the "no error" sentinel inside the core
 * and never reaches an error event.
 */
export type RlottieErrorCode =
  | 'INVALID_SOURCE'
  | 'SOURCE_NOT_FOUND'
  | 'SOURCE_TOO_LARGE'
  | 'PARSE_FAILED'
  | 'INVALID_DIMENSIONS'
  | 'ALLOCATION_FAILED'
  | 'RENDER_FAILED'
  | 'RELEASED';

// --- Event payloads ---------------------------------------------------------

/** A named frame segment declared in the Lottie source. */
export interface RlottieMarker {
  name: string;
  startFrame: number;
  endFrame: number;
}

export interface RlottieLoadedEvent {
  /** Intrinsic composition size in pixels, as authored. */
  width: number;
  height: number;
  /** Seconds. */
  duration: number;
  frameRate: number;
  totalFrames: number;
  markers: RlottieMarker[];
}

export interface RlottieErrorEvent {
  code: RlottieErrorCode;
  /** Developer-facing detail. Not localized; do not show it to end users. */
  message: string;
}

/** The four lifecycle events carry no payload (`{}` on the native side). */
export type RlottieLifecycleEvent = Record<string, never>;

// --- Props ------------------------------------------------------------------

export type RlottieResizeMode = 'contain' | 'cover' | 'stretch' | 'center';

export type RlottieCacheStrategy = 'none' | 'model';

export interface RlottieColorOverride {
  /** rlottie key path, e.g. `"layer.group.fill"`. */
  keyPath: string;
  /** `#RRGGBB` or `#AARRGGBB`. Alpha is parsed but currently DISCARDED — the
   *  core's `setColor` has no alpha parameter. */
  color: string;
}

/** Phase 6 addition. Parallel in shape to `RlottieColorOverride`. */
export interface RlottieOpacityOverride {
  /** rlottie key path, same syntax as `RlottieColorOverride.keyPath`. */
  keyPath: string;
  /** 0..1. Out-of-range values are clamped natively rather than rejected. */
  opacity: number;
}

/** Phase 6 addition. Parallel in shape to `RlottieColorOverride`. */
export interface RlottieStrokeWidthOverride {
  /** rlottie key path, same syntax as `RlottieColorOverride.keyPath`. */
  keyPath: string;
  /** Pixels, > 0. Out-of-range values are clamped natively rather than rejected. */
  width: number;
}

export interface RlottieViewProps extends ViewProps {
  source: RlottieSource;

  /**
   * Default `false`. Opt-in gate for a `source.uri` on `https://` (plan §16).
   *
   * v1 STUB (Chunk 5.4): this only changes which rejection message
   * `normalizeSource` returns. Remote loading itself — the actual fetch — is
   * v1.1 scope; the C++ core never performs network I/O (plan §12/§16), so
   * setting this flag today does not make a remote source load. It exists now
   * so the prop name and default are locked before the download path lands.
   *
   * `http://` (no TLS) is rejected regardless of this flag — HTTPS-only is
   * non-negotiable (plan §16), not something a consumer can opt out of.
   */
  allowRemoteSources?: boolean;

  autoPlay?: boolean;
  loop?: boolean;
  /** With `loop`: 0 (default) is infinite; N > 0 is N extra plays. */
  repeatCount?: number;
  /** 1.0 default. Negative plays in reverse; 0 holds the current frame. */
  speed?: number;

  /** 0..1. Seeks; does not start playback. */
  progress?: number;
  startFrame?: number;
  /** 0 (default) means "last frame". */
  endFrame?: number;

  /**
   * Default `'contain'`.
   *
   * KNOWN GAP: honoured on iOS (via `layer.contentsGravity`); accepted and
   * validated but currently a no-op on Android, whose draw path always
   * stretches to the view bounds.
   */
  resizeMode?: RlottieResizeMode;

  /** Multiplies the render surface resolution. 1.0 default. */
  renderScale?: number;

  /** Default `true`. Pauses playback while the app is backgrounded. */
  pauseWhenInactive?: boolean;

  /**
   * Default `'model'`.
   *
   * KNOWN GAP: plumbed on iOS; accepted but currently a no-op on Android, whose
   * source natives take no cache flag yet.
   */
  cacheStrategy?: RlottieCacheStrategy;

  colorOverrides?: RlottieColorOverride[];

  /** Phase 6 addition. Applies on the same serialized render worker as rendering. */
  opacityOverrides?: RlottieOpacityOverride[];
  /** Phase 6 addition. Applies on the same serialized render worker as rendering. */
  strokeWidthOverrides?: RlottieStrokeWidthOverride[];

  onAnimationLoaded?: (event: NativeSyntheticEvent<RlottieLoadedEvent>) => void;
  onAnimationError?: (event: NativeSyntheticEvent<RlottieErrorEvent>) => void;
  onAnimationStart?: (
    event: NativeSyntheticEvent<RlottieLifecycleEvent>,
  ) => void;
  onAnimationPause?: (
    event: NativeSyntheticEvent<RlottieLifecycleEvent>,
  ) => void;
  onAnimationLoop?: (
    event: NativeSyntheticEvent<RlottieLifecycleEvent>,
  ) => void;
  onAnimationFinish?: (
    event: NativeSyntheticEvent<RlottieLifecycleEvent>,
  ) => void;
}

// There is deliberately no `onFrame` prop. A per-frame bridge event is the exact
// performance problem this library exists to avoid (plan §11) — frames never
// cross the bridge.

// --- Imperative ref ---------------------------------------------------------

export interface RlottiePlayOptions {
  /**
   * One-off segment for THIS play only; does not change the configured range.
   *
   * When `marker` is set it WINS and `startFrame`/`endFrame` are ignored
   * (Phase 6, docs/bridge-contract.md "Command id 9 — playMarker") — mixing a
   * named segment with an explicit frame range has no coherent meaning, so
   * this is a documented precedence rather than a silent merge.
   */
  startFrame?: number;
  endFrame?: number;
  /**
   * Plays the frame segment of a named marker from the Lottie source (see
   * `RlottieMarker`, delivered via `onAnimationLoaded`). Takes precedence over
   * `startFrame`/`endFrame` when both are provided. An unknown marker name is
   * a silent no-op, not an error.
   */
  marker?: string;
}

export interface RlottieViewRef {
  play(options?: RlottiePlayOptions): void;
  pause(): void;
  resume(): void;
  stop(): void;
  reset(): void;
  /** `progress` is clamped to 0..1 natively. */
  seekToProgress(progress: number): void;
  seekToFrame(frame: number): void;
  setSpeed(speed: number): void;
}

// --- Global module ----------------------------------------------------------

export interface RlottieNativeVersion {
  /** Full commit SHA of the vendored rlottie (`cpp/RlottieVersion.h`). */
  rlottieCommit: string;
  /** Model-cache size currently in effect. */
  modelCacheSize: number;
}

export interface RlottieConfigureOptions {
  /** Max number of parsed animation models rlottie keeps cached. */
  modelCacheSize?: number;
}
