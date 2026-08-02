// TurboModule spec for the global `RlottieModule`.
// Filename and the `Spec` interface name are load-bearing for codegen.
// See docs/new-architecture-design.md §1.1 and §2.2.

import type {CodegenTypes, TurboModule} from 'react-native';
import {TurboModuleRegistry} from 'react-native';

export interface Spec extends TurboModule {
  configure(options: CodegenTypes.UnsafeObject): Promise<void>;
  clearModelCache(): Promise<void>;
  getNativeVersion(): Promise<{
    rlottieCommit: string;
    modelCacheSize: number;
  }>;
}

export default TurboModuleRegistry.get<Spec>('RlottieModule');
