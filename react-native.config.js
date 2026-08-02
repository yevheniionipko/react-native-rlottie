// Autolinking metadata for React Native (Legacy Architecture).
//
// Both platforms use this package's default layout, so the platform entries are
// intentionally empty rather than redundantly restating the defaults:
//   - iOS discovers `react-native-rlottie.podspec` at the package root;
//   - Android discovers `android/` and its `build.gradle`.
//
// Declaring the keys at all is what matters: it tells the CLI this package
// provides native code for both platforms, so `pod install` and Gradle pick it
// up without the consumer editing anything.
//
// This file describes the package as a DEPENDENCY. It deliberately has no
// `project` or `commands` section — those configure a consuming app, not a
// library.
module.exports = {
  dependency: {
    platforms: {
      ios: {},
      android: {},
    },
  },
};
