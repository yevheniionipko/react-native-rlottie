// Chunk 4.2 — the native component handle.
//
// SIGNATURE FROZEN: Chunk 4.3 (`RlottieView.tsx`) is written against the
// exports below, so their names and types must not change without updating that
// file in the same commit. The BODIES are Chunk 4.2's to implement.

import type {HostComponent, ViewProps} from 'react-native';
import {requireNativeComponent} from 'react-native';

import type {
  NormalizedRlottieSource,
  RlottieCacheStrategy,
  RlottieColorOverride,
  RlottieErrorEvent,
  RlottieLifecycleEvent,
  RlottieLoadedEvent,
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

/** The host component. `RlottieView` (Chunk 4.3) is the only thing that renders it. */
export const RlottieNativeComponent: HostComponent<RlottieNativeProps> =
  requireNativeComponent<RlottieNativeProps>(RLOTTIE_COMPONENT_NAME);

export default RlottieNativeComponent;
