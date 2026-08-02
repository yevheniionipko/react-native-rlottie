// Chunk 9.1 — the Fabric component spec + Commands.
//
// This file is the codegen INPUT for the New Architecture path. It is parsed
// by `@react-native/codegen` (via the Babel plugin under the old architecture,
// and via the CLI's `codegen` command for Fabric/TurboModules) to generate:
//   - iOS: `RlottieViewComponentDescriptor`, `RlottieViewProps`,
//     `RlottieViewEventEmitter`, `RCTRlottieViewViewProtocol`.
//   - Android: `RlottieViewManagerInterface`/`RlottieViewManagerDelegate` in
//     `com.facebook.react.viewmanagers`.
//
// FILENAME IS LOAD-BEARING (docs/new-architecture-design.md §1.1):
// `@react-native/babel-plugin-codegen` only rewrites `codegenNativeComponent`
// in a file matching `/NativeComponent\.(js|ts)$/` — not `.tsx`. Do not rename
// or convert to `.tsx`.
//
// This is a SPEC, not the public API: `docs/bridge-contract.md` remains the
// frozen source of truth for prop/event/command names, and `src/types.ts` /
// `src/RlottieView.tsx` are what consumers actually use. Every prop, event,
// and command below must match that document exactly. Notable deliberate
// choices (see the design doc for the full reasoning):
//
//   - `resizeMode` / `cacheStrategy` are `WithDefault<string, ...>`, NOT
//     string-literal unions — an unrecognized value in a generated enum's
//     `fromRawValue` calls `abort()`, an uncatchable process kill from a
//     background-thread prop parse. Both native adapters already validate
//     and fall back gracefully; the union types live in `src/types.ts` for
//     the public API only.
//   - The four lifecycle events and the two metrics-adjacent bookkeeping
//     concerns match `docs/bridge-contract.md` exactly.
//   - `allowRemoteSources` is deliberately ABSENT: it is a JS-only gate
//     consumed by `normalizeSource` and must never cross the bridge on either
//     architecture (see "Remote loading policy" in the bridge contract).
//   - Metrics counters (`framesRendered`, `framesDropped`, `bufferAllocCount`,
//     `peakBufferBytes`, `uiStallCount`, `uiStallMaxMs`) are `Double`, not
//     `Int32` — this is a deliberate contract amendment fixing the
//     Android `putInt` (saturating 32-bit) vs. iOS `uint64_t` divergence
//     CLAUDE.md records. `docs/bridge-contract.md`'s Phase 7 table changes
//     `int` -> `double` for those fields in the same chunk that adds this
//     file.

import type * as React from 'react';
import type {CodegenTypes, HostComponent, ViewProps} from 'react-native';
import codegenNativeCommands from 'react-native/Libraries/Utilities/codegenNativeCommands';
import codegenNativeComponent from 'react-native/Libraries/Utilities/codegenNativeComponent';

// --- source -------------------------------------------------------------
//
// A typed object, not UnsafeMixed, so Android keeps taking a plain
// `@Nullable ReadableMap` — byte-identical to the existing
// `@ReactProp(name = "source") fun setSource(view, source: ReadableMap?)` —
// and `RlottieSourceResolver`/`RNRlottieSourceResolver` are untouched. See
// docs/new-architecture-design.md §3.2.1 for the full reasoning, including
// the mandatory `cacheKey`-based equality rule both platforms must apply
// before re-resolving a source on every Fabric commit.

type SourceProp = Readonly<{
  json?: string;
  uri?: string;
  path?: string;
  cacheKey?: string;
  resourcePath?: string;
}>;

// --- override arrays ------------------------------------------------------

type ColorOverride = Readonly<{
  keyPath: string;
  color: string;
}>;

type OpacityOverride = Readonly<{
  keyPath: string;
  opacity: CodegenTypes.Double;
}>;

type StrokeWidthOverride = Readonly<{
  keyPath: string;
  width: CodegenTypes.Double;
}>;

// --- event payloads ---------------------------------------------------------

type OnAnimationLoadedEvent = Readonly<{
  width: CodegenTypes.Int32;
  height: CodegenTypes.Int32;
  duration: CodegenTypes.Double;
  frameRate: CodegenTypes.Double;
  totalFrames: CodegenTypes.Int32;
  // Event payloads (unlike props) go through a narrower codegen path with two
  // verified constraints, both confirmed by actually running codegen (per the
  // Chunk 9.1 acceptance criteria — divergences from this design doc's §3.1
  // sketch, recorded here rather than only in the sketch):
  //   1. Plain array syntax only — `ReadonlyArray<T>` throws "Unable to
  //      determine event type" (`events.js`'s `getPropertyType` switches on
  //      `TSArrayType` but has no case for the `ReadonlyArray` type-reference
  //      name).
  //   2. The element type must be inlined, not a named type alias —
  //      `extractArrayElementType` resolves `TSTypeLiteral` (an inline
  //      `{...}`) but does not look up a referenced alias by name, so a
  //      separate `type Marker = {...}` throws "Unrecognized Marker for
  //      Array markers in events".
  markers: {
    name: string;
    startFrame: CodegenTypes.Int32;
    endFrame: CodegenTypes.Int32;
  }[];
}>;

type OnAnimationErrorEvent = Readonly<{
  code: string;
  message: string;
}>;

type OnMetricsEvent = Readonly<{
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

// --- props ------------------------------------------------------------------

export interface NativeProps extends ViewProps {
  source?: SourceProp;

  autoPlay?: CodegenTypes.WithDefault<boolean, false>;
  loop?: CodegenTypes.WithDefault<boolean, false>;
  /** With `loop`: 0 (default) is infinite; N > 0 is N extra plays. */
  repeatCount?: CodegenTypes.WithDefault<CodegenTypes.Int32, 0>;
  /** Negative plays in reverse; 0 holds the current frame. */
  speed?: CodegenTypes.WithDefault<CodegenTypes.Double, 1.0>;
  /** 0..1. Seeks; does not start playback. Seek only on change from oldProps
   *  (docs/new-architecture-design.md §3.2.4) — there is no "unset" sentinel
   *  under Fabric. */
  progress?: CodegenTypes.WithDefault<CodegenTypes.Double, 0.0>;
  startFrame?: CodegenTypes.WithDefault<CodegenTypes.Int32, 0>;
  /** 0 (default) means "last frame". */
  endFrame?: CodegenTypes.WithDefault<CodegenTypes.Int32, 0>;
  /**
   * `contain` | `cover` | `stretch` | `center`, default `contain`.
   *
   * DELIBERATELY `string`, not a string-literal union — see
   * docs/new-architecture-design.md §3.2.3. A generated enum aborts the
   * process on an unrecognized value; both native adapters already validate
   * and fall back gracefully, and must keep doing so.
   */
  resizeMode?: CodegenTypes.WithDefault<string, 'contain'>;
  renderScale?: CodegenTypes.WithDefault<CodegenTypes.Double, 1.0>;
  pauseWhenInactive?: CodegenTypes.WithDefault<boolean, true>;
  /**
   * `none` | `model`, default `model`.
   *
   * DELIBERATELY `string`, not a string-literal union — same reasoning as
   * `resizeMode` above.
   */
  cacheStrategy?: CodegenTypes.WithDefault<string, 'model'>;
  colorOverrides?: ReadonlyArray<ColorOverride>;
  opacityOverrides?: ReadonlyArray<OpacityOverride>;
  strokeWidthOverrides?: ReadonlyArray<StrokeWidthOverride>;
  metricsEnabled?: CodegenTypes.WithDefault<boolean, false>;

  onAnimationLoaded?: CodegenTypes.DirectEventHandler<OnAnimationLoadedEvent>;
  onAnimationError?: CodegenTypes.DirectEventHandler<OnAnimationErrorEvent>;
  onAnimationStart?: CodegenTypes.DirectEventHandler<null>;
  onAnimationPause?: CodegenTypes.DirectEventHandler<null>;
  onAnimationLoop?: CodegenTypes.DirectEventHandler<null>;
  onAnimationFinish?: CodegenTypes.DirectEventHandler<null>;
  onMetrics?: CodegenTypes.DirectEventHandler<OnMetricsEvent>;
}

// --- commands -----------------------------------------------------------
//
// Under Fabric, commands dispatch BY NAME, never by numeric id — see
// docs/new-architecture-design.md §3.3. The numeric ids frozen in
// `docs/bridge-contract.md` remain authoritative for the Legacy path only;
// nothing here is declaration-order sensitive, and no reserved slot is
// needed (that is a Legacy-iOS-only concern).

interface NativeCommands {
  play: (
    ref: React.ElementRef<HostComponent<NativeProps>>,
    startFrame: CodegenTypes.Int32,
    endFrame: CodegenTypes.Int32,
  ) => void;
  pause: (ref: React.ElementRef<HostComponent<NativeProps>>) => void;
  resume: (ref: React.ElementRef<HostComponent<NativeProps>>) => void;
  stop: (ref: React.ElementRef<HostComponent<NativeProps>>) => void;
  reset: (ref: React.ElementRef<HostComponent<NativeProps>>) => void;
  seekToProgress: (
    ref: React.ElementRef<HostComponent<NativeProps>>,
    progress: CodegenTypes.Double,
  ) => void;
  seekToFrame: (
    ref: React.ElementRef<HostComponent<NativeProps>>,
    frame: CodegenTypes.Int32,
  ) => void;
  setSpeed: (
    ref: React.ElementRef<HostComponent<NativeProps>>,
    speed: CodegenTypes.Double,
  ) => void;
  playMarker: (
    ref: React.ElementRef<HostComponent<NativeProps>>,
    name: string,
  ) => void;
}

export const Commands: NativeCommands = codegenNativeCommands<NativeCommands>({
  supportedCommands: [
    'play',
    'pause',
    'resume',
    'stop',
    'reset',
    'seekToProgress',
    'seekToFrame',
    'setSpeed',
    'playMarker',
  ],
});

export default codegenNativeComponent<NativeProps>('RlottieView');
