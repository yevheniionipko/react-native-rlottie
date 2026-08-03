// TurboModule spec for the global `RlottieModule`.
// Filename and the `Spec` interface name are load-bearing for codegen.
// See docs/new-architecture-design.md §1.1 and §2.2.
//
// Named imports, not the `CodegenTypes.X` namespaced form: RN's codegen
// cannot resolve a qualified type reference until 0.80.0 (see the "Minimum
// React Native version" section of that doc), and this form works down to
// RN 0.76.

import type {TurboModule} from 'react-native';
import type {UnsafeObject} from 'react-native/Libraries/Types/CodegenTypes';
import {TurboModuleRegistry} from 'react-native';

export interface Spec extends TurboModule {
  configure(options: UnsafeObject): Promise<void>;
  clearModelCache(): Promise<void>;
  getNativeVersion(): Promise<{
    rlottieCommit: string;
    modelCacheSize: number;
  }>;
}

export default TurboModuleRegistry.get<Spec>('RlottieModule');
