// Autolinking metadata for React Native (both architectures).
//
// iOS stays empty — it discovers `react-native-rlottie.podspec` at the
// package root with no extra hints needed, on both architectures.
//
// Android is pinned rather than left to the CLI's `findComponentDescriptors`
// heuristic (docs/new-architecture-design.md §1.3). That heuristic globs the
// package's JS/TS and regex-scans for `codegenNativeComponent`; if it ever
// misses, the app's generated `autolinking.cpp` silently omits our
// `ComponentDescriptor` registration and the component renders as an
// unimplemented view under the New Architecture. Pinning these three values
// removes the heuristic from the failure surface:
//   - `libraryName` must match `codegenConfig.name` in `package.json`.
//   - `componentDescriptors` names the ONE Fabric component this package
//     exports.
//   - `cmakeListsPath` is where the app's own codegen run (not this package's
//     `android/src/main/cpp/CMakeLists.txt`) writes the generated
//     `react_codegen_RNRlottieSpec` target.
// The cost is one more place to update if the component is ever renamed —
// a fair trade against a silent mis-registration.
//
// This file describes the package as a DEPENDENCY. It deliberately has no
// `project` or `commands` section — those configure a consuming app, not a
// library.
module.exports = {
  dependency: {
    platforms: {
      ios: {},
      android: {
        libraryName: 'RNRlottieSpec',
        componentDescriptors: ['RlottieViewComponentDescriptor'],
        cmakeListsPath: 'build/generated/source/codegen/jni/CMakeLists.txt',
      },
    },
  },
};
