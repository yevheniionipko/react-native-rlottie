// Chunk 4.4 — the global `Rlottie` module.
// Chunk 9.9 — resolved through the codegen spec's default export
// (`TurboModuleRegistry.get`, which itself falls back to `NativeModules` on
// the old architecture) instead of `NativeModules[MODULE_NAME]` directly. One
// code path for both architectures; see docs/new-architecture-design.md §2.5.
//
// Process-wide operations ONLY (plan §15): the rlottie model-cache size and the
// vendored native version. Per-view playback never routes through here — that is
// `RlottieView` + the command dispatcher. Keeping this module tiny is the point:
// a native module is a bridge round-trip per call, which is exactly what a
// per-frame API must not be.
//
// All three methods are Promise-based on BOTH platforms
// (`ios/RNRlottieModule.mm` uses RCT_REMAP_METHOD with a resolver;
// `android/.../RlottieModule.kt` takes a trailing `Promise`), so `await` behaves
// identically. See docs/bridge-contract.md "Global module".

import {NativeModules, TurboModuleRegistry} from 'react-native';

import RlottieModuleSpec from './specs/NativeRlottieModule';
import type {RlottieConfigureOptions, RlottieNativeVersion} from './types';

/** Must match iOS `RCT_EXPORT_MODULE(RlottieModule)` and Kotlin `NAME`. */
const MODULE_NAME = 'RlottieModule';

interface RlottieNativeModule {
  configure(options: RlottieConfigureOptions): Promise<void>;
  clearModelCache(): Promise<void>;
  getNativeVersion(): Promise<RlottieNativeVersion>;
}

const LINKING_ERROR =
  `The native module "${MODULE_NAME}" is not available.\n\n` +
  'react-native-rlottie ships native code, so it cannot work in a JS-only ' +
  'environment. Check that you have:\n' +
  '  - run `pod install` (iOS) after installing the package;\n' +
  '  - rebuilt the app (a Metro reload is not enough — native code changed);\n' +
  '  - not disabled autolinking for this package.\n' +
  'It is also expected to be missing under Jest or any renderer without the ' +
  'native runtime.';

/**
 * Resolves the native module, or returns null.
 *
 * The lookup is redone on every call, never cached. The spec module's default
 * export is `TurboModuleRegistry.get(...)` evaluated at ITS import time, which
 * under the New Architecture can run before the TurboModule is resolvable — and
 * a null captured then would be permanent. Falling through to a live
 * `TurboModuleRegistry.get` and then `NativeModules` makes the first successful
 * resolution win whenever it happens. See docs/new-architecture-design.md §2.5.
 *
 * `configure`'s spec argument is `UnsafeObject` (§2.2), so the typed signature
 * above is only enforced at this module's boundary.
 */
function resolveNative(): RlottieNativeModule | null {
  const fromSpec = RlottieModuleSpec as RlottieNativeModule | null;
  if (fromSpec) {
    return fromSpec;
  }
  const fromRegistry = TurboModuleRegistry.get<never>(
    MODULE_NAME,
  ) as RlottieNativeModule | null;
  if (fromRegistry) {
    return fromRegistry;
  }
  return (NativeModules[MODULE_NAME] as RlottieNativeModule) ?? null;
}

/**
 * Resolves the native module, or throws a diagnostic error.
 *
 * Deliberately lazy (per call) rather than throwing at import time: this module
 * is re-exported from the package entry point, so throwing on import would take
 * down any consumer that merely imports `RlottieView` in an environment without
 * the native runtime — a test file, a storybook, a web build. Failing at the
 * call site keeps the blast radius to the code that actually needs native.
 */
function requireNative(): RlottieNativeModule {
  const nativeModule = resolveNative();
  if (!nativeModule) {
    throw new Error(LINKING_ERROR);
  }
  return nativeModule;
}

/**
 * Global configuration. Currently only the rlottie model-cache size, which
 * bounds how many PARSED animation models rlottie keeps in memory
 * process-wide (`cpp/ModelCacheController.h`).
 *
 * A missing/negative `modelCacheSize` is a no-op natively rather than a clamp
 * to 0 — clamping would silently disable caching on a bad call. Read back what
 * actually took effect with `getNativeVersion()`.
 */
export function configure(options: RlottieConfigureOptions): Promise<void> {
  return requireNative().configure(options ?? {});
}

/** Flushes the model cache, preserving the configured size. */
export function clearModelCache(): Promise<void> {
  return requireNative().clearModelCache();
}

/**
 * The pinned rlottie commit (`cpp/RlottieVersion.h`) and the model-cache size
 * currently in effect. Useful for bug reports — the rlottie revision determines
 * rendering behaviour.
 */
export function getNativeVersion(): Promise<RlottieNativeVersion> {
  return requireNative().getNativeVersion();
}

/** True when the native module is linked. Lets callers degrade gracefully. */
export function isAvailable(): boolean {
  return resolveNative() != null;
}

export const Rlottie = {
  configure,
  clearModelCache,
  getNativeVersion,
  isAvailable,
};

export default Rlottie;
