// Autolinking metadata for this package as a DEPENDENCY (both architectures).
// Android values are pinned rather than left to the CLI's heuristic; see
// docs/new-architecture-design.md §1.3.
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
