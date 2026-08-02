// Chunk 9.1 — the TurboModule spec for the global `RlottieModule`.
//
// FILENAME AND EXPORTED-INTERFACE NAME ARE BOTH LOAD-BEARING
// (docs/new-architecture-design.md §1.1): the module-side codegen requires the
// file to be named `Native<Name>.ts` and the exported interface to be named
// exactly `Spec`
// (node_modules/@react-native/codegen/lib/parsers/typescript/parser.js:122-128
// rejects anything else).
//
// This module exposes only process-global operations (plan §15 /
// docs/bridge-contract.md "Global module") — never per-view playback, which
// stays on the Fabric component / Legacy ViewManager.
//
// See docs/new-architecture-design.md §2.2 for the three deliberate choices
// behind this exact shape:
//   - `configure`'s argument is `UnsafeObject`, not a typed object, so one
//     Obj-C/Kotlin method body satisfies both the old- and new-architecture
//     surfaces (a typed object would force codegen to emit a struct that
//     cannot be expressed as an old-architecture `RCT_EXPORT_METHOD`).
//   - `getNativeVersion`'s return type IS typed — a Promise resolves through
//     an untyped `id`/`Object` on both platforms regardless, so typing it
//     constrains nothing native while giving JS/generated types something
//     real.
//   - `TurboModuleRegistry.get`, not `getEnforcing` — `getEnforcing` throws at
//     spec-import time when the native module is absent, which would break
//     importing `RlottieView` in a test/storybook/web build with no native
//     runtime. `get` returns null, preserving `src/RlottieModule.ts`'s lazy
//     resolution and its `isAvailable()` check.

import type {CodegenTypes, TurboModule} from 'react-native';
import {TurboModuleRegistry} from 'react-native';

export interface Spec extends TurboModule {
  /**
   * `{modelCacheSize?: number}` on the wire — deliberately `UnsafeObject`
   * rather than a typed object; see the header comment above.
   */
  configure(options: CodegenTypes.UnsafeObject): Promise<void>;
  clearModelCache(): Promise<void>;
  getNativeVersion(): Promise<{
    rlottieCommit: string;
    modelCacheSize: number;
  }>;
}

export default TurboModuleRegistry.get<Spec>('RlottieModule');
