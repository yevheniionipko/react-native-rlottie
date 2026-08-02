// react-native-rlottie — the public entry point.
//
// Everything a consumer needs is exported here, and nothing else is supported:
// importing from a deep path (`react-native-rlottie/src/commands`) reaches into
// internals that change without notice. In particular `src/commands.ts` is the
// only place `UIManager`/`findNodeHandle` may be touched, and it stays internal
// so consumers cannot hand-roll a dispatch that bypasses the contract's guards
// (CLAUDE.md's layering rule: consumers never touch platform methods).

// --- The component ----------------------------------------------------------

export {RlottieView, default} from './RlottieView';

// --- The global module ------------------------------------------------------

export {
  Rlottie,
  configure,
  clearModelCache,
  getNativeVersion,
  isAvailable,
} from './RlottieModule';

// --- Public types -----------------------------------------------------------

export type {
  // Sources
  RlottieSource,
  RlottieJsonObject,
  NormalizedRlottieSource,
  // Errors
  RlottieErrorCode,
  // Event payloads
  RlottieMarker,
  RlottieLoadedEvent,
  RlottieErrorEvent,
  RlottieLifecycleEvent,
  // Props
  RlottieViewProps,
  RlottieResizeMode,
  RlottieCacheStrategy,
  RlottieColorOverride,
  RlottieOpacityOverride,
  RlottieStrokeWidthOverride,
  // Imperative ref
  RlottieViewRef,
  RlottiePlayOptions,
  // Global module
  RlottieNativeVersion,
  RlottieConfigureOptions,
} from './types';

// The stable numeric command ids are exported for reference and for consumers
// writing their own native integrations — NOT as a dispatch mechanism. Sending
// one of these to `UIManager.dispatchViewManagerCommand` yourself reintroduces
// the iOS raw-array-index fragility that `src/commands.ts` exists to route
// around (see docs/bridge-contract.md). Use the `RlottieViewRef` methods.
export {RlottieCommand} from './commands';
