// Chunk 4.2 — the native component handle.
// Chunk 9.9 — sourced from the codegen spec (docs/new-architecture-design.md
// §3.8) instead of `requireNativeComponent` directly, so Fabric apps get the
// generated component while Legacy apps still resolve the native ViewManager's
// own view config (see the doc section for why that's safe unchanged).
//
// SIGNATURE FROZEN: Chunk 4.3 (`RlottieView.tsx`) is written against the
// exports below, so their names and types must not change without updating that
// file in the same commit. The BODIES are Chunk 4.2's to implement.

import type {HostComponent, ViewProps} from 'react-native';

import RlottieViewSpec from './specs/RlottieViewNativeComponent';
import type {
  NormalizedRlottieSource,
  RlottieCacheStrategy,
  RlottieColorOverride,
  RlottieErrorEvent,
  RlottieLifecycleEvent,
  RlottieLoadedEvent,
  RlottieMetricsEvent,
  RlottieOpacityOverride,
  RlottieResizeMode,
  RlottieStrokeWidthOverride,
} from './types';

import type {NativeSyntheticEvent} from 'react-native';

/**
 * Must match `docs/bridge-contract.md` — iOS `RCT_EXPORT_MODULE(RlottieView)`
 * and Android `RlottieViewManager.getName()`.
 */
export const RLOTTIE_COMPONENT_NAME = 'RlottieView';

/**
 * Exactly what crosses the bridge. This is NOT the public prop type: `source`
 * is already normalized (Chunk 4.1), and every optional here is resolved to a
 * concrete value by `RlottieView` before it reaches native, so the native
 * defaults and the documented defaults cannot drift apart.
 */
export interface RlottieNativeProps extends ViewProps {
  source?: NormalizedRlottieSource;

  autoPlay?: boolean;
  loop?: boolean;
  repeatCount?: number;
  speed?: number;
  progress?: number;
  startFrame?: number;
  endFrame?: number;
  resizeMode?: RlottieResizeMode;
  renderScale?: number;
  pauseWhenInactive?: boolean;
  cacheStrategy?: RlottieCacheStrategy;
  colorOverrides?: RlottieColorOverride[];
  opacityOverrides?: RlottieOpacityOverride[];
  strokeWidthOverrides?: RlottieStrokeWidthOverride[];
  metricsEnabled?: boolean;

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
  onMetrics?: (event: NativeSyntheticEvent<RlottieMetricsEvent>) => void;
}

/**
 * The host component. `RlottieView` (Chunk 4.3) is the only thing that renders
 * it. `RlottieNativeProps` (above) is this module's own prop type, kept
 * structurally compatible with the spec's generated `NativeProps` rather than
 * imported from it, so this file's public shape doesn't move with codegen
 * output.
 */
export const RlottieNativeComponent =
  RlottieViewSpec as unknown as HostComponent<RlottieNativeProps>;

export default RlottieNativeComponent;
