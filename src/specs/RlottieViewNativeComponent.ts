// Fabric component spec for `RlottieView`.
// Filename is load-bearing for codegen (must end `NativeComponent.ts`, not
// `.tsx`). See docs/new-architecture-design.md §1.1, §3.1-§3.4.

import type * as React from 'react';
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

type OnAnimationLoadedEvent = Readonly<{
  width: CodegenTypes.Int32;
  height: CodegenTypes.Int32;
  duration: CodegenTypes.Double;
  frameRate: CodegenTypes.Double;
  totalFrames: CodegenTypes.Int32;
  // Event arrays must be `T[]` with an inlined element type; see design doc §5.
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

export interface NativeProps extends ViewProps {
  source?: SourceProp;

  autoPlay?: CodegenTypes.WithDefault<boolean, false>;
  loop?: CodegenTypes.WithDefault<boolean, false>;
  repeatCount?: CodegenTypes.WithDefault<CodegenTypes.Int32, 0>;
  speed?: CodegenTypes.WithDefault<CodegenTypes.Double, 1.0>;
  progress?: CodegenTypes.WithDefault<CodegenTypes.Double, 0.0>;
  startFrame?: CodegenTypes.WithDefault<CodegenTypes.Int32, 0>;
  endFrame?: CodegenTypes.WithDefault<CodegenTypes.Int32, 0>;
  // `string`, not a string-literal union, on purpose; see design doc §3.2.3.
  resizeMode?: CodegenTypes.WithDefault<string, 'contain'>;
  renderScale?: CodegenTypes.WithDefault<CodegenTypes.Double, 1.0>;
  pauseWhenInactive?: CodegenTypes.WithDefault<boolean, true>;
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
  // NOT `setSpeed`: that collides with the `speed` prop's generated setter on
  // Android. See docs/new-architecture-design.md §3.3.
  setPlaybackSpeed: (
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
    'setPlaybackSpeed',
    'playMarker',
  ],
});

export default codegenNativeComponent<NativeProps>('RlottieView');
