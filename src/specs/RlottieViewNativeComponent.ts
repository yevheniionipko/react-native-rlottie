// Fabric component spec for `RlottieView`.
// Filename is load-bearing for codegen (must end `NativeComponent.ts`, not
// `.tsx`). See docs/new-architecture-design.md §1.1, §3.1-§3.4.

import type * as React from 'react';
import type {HostComponent, ViewProps} from 'react-native';
import type {
  Double,
  Int32,
  DirectEventHandler,
  WithDefault,
} from 'react-native/Libraries/Types/CodegenTypes';
import codegenNativeCommands from 'react-native/Libraries/Utilities/codegenNativeCommands';
import codegenNativeComponent from 'react-native/Libraries/Utilities/codegenNativeComponent';

type SourceProp = Readonly<{
  json?: string;
  uri?: string;
  path?: string;
  cacheKey?: string;
  resourcePath?: string;
}>;

type ColorOverride = Readonly<{
  keyPath: string;
  color: string;
}>;

type OpacityOverride = Readonly<{
  keyPath: string;
  opacity: Double;
}>;

type StrokeWidthOverride = Readonly<{
  keyPath: string;
  width: Double;
}>;

type OnAnimationLoadedEvent = Readonly<{
  width: Int32;
  height: Int32;
  duration: Double;
  frameRate: Double;
  totalFrames: Int32;
  // Event arrays must be `T[]` with an inlined element type; see design doc §5.
  markers: {
    name: string;
    startFrame: Int32;
    endFrame: Int32;
  }[];
}>;

type OnAnimationErrorEvent = Readonly<{
  code: string;
  message: string;
}>;

type OnMetricsEvent = Readonly<{
  parseMs: Double;
  firstFrameMs: Double;
  renderP50Ms: Double;
  renderP95Ms: Double;
  renderP99Ms: Double;
  framesRendered: Double;
  framesDropped: Double;
  bufferAllocCount: Double;
  peakBufferBytes: Double;
  uiStallCount: Double;
  uiStallMaxMs: Double;
}>;

export interface NativeProps extends ViewProps {
  source?: SourceProp;

  autoPlay?: WithDefault<boolean, false>;
  loop?: WithDefault<boolean, false>;
  repeatCount?: WithDefault<Int32, 0>;
  speed?: WithDefault<Double, 1.0>;
  progress?: WithDefault<Double, 0.0>;
  startFrame?: WithDefault<Int32, 0>;
  endFrame?: WithDefault<Int32, 0>;
  // `string`, not a string-literal union, on purpose; see design doc §3.2.3.
  resizeMode?: WithDefault<string, 'contain'>;
  renderScale?: WithDefault<Double, 1.0>;
  pauseWhenInactive?: WithDefault<boolean, true>;
  cacheStrategy?: WithDefault<string, 'model'>;
  colorOverrides?: ReadonlyArray<ColorOverride>;
  opacityOverrides?: ReadonlyArray<OpacityOverride>;
  strokeWidthOverrides?: ReadonlyArray<StrokeWidthOverride>;
  metricsEnabled?: WithDefault<boolean, false>;

  onAnimationLoaded?: DirectEventHandler<OnAnimationLoadedEvent>;
  onAnimationError?: DirectEventHandler<OnAnimationErrorEvent>;
  onAnimationStart?: DirectEventHandler<null>;
  onAnimationPause?: DirectEventHandler<null>;
  onAnimationLoop?: DirectEventHandler<null>;
  onAnimationFinish?: DirectEventHandler<null>;
  onMetrics?: DirectEventHandler<OnMetricsEvent>;
}

interface NativeCommands {
  play: (
    ref: React.ElementRef<HostComponent<NativeProps>>,
    startFrame: Int32,
    endFrame: Int32,
  ) => void;
  pause: (ref: React.ElementRef<HostComponent<NativeProps>>) => void;
  resume: (ref: React.ElementRef<HostComponent<NativeProps>>) => void;
  stop: (ref: React.ElementRef<HostComponent<NativeProps>>) => void;
  reset: (ref: React.ElementRef<HostComponent<NativeProps>>) => void;
  seekToProgress: (
    ref: React.ElementRef<HostComponent<NativeProps>>,
    progress: Double,
  ) => void;
  seekToFrame: (
    ref: React.ElementRef<HostComponent<NativeProps>>,
    frame: Int32,
  ) => void;
  // NOT `setSpeed`: that collides with the `speed` prop's generated setter on
  // Android. See docs/new-architecture-design.md §3.3.
  setPlaybackSpeed: (
    ref: React.ElementRef<HostComponent<NativeProps>>,
    speed: Double,
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
    'setPlaybackSpeed',
    'playMarker',
  ],
});

export default codegenNativeComponent<NativeProps>('RlottieView');
