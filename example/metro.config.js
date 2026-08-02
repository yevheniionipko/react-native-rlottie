const path = require('path');
const { getDefaultConfig, mergeConfig } = require('@react-native/metro-config');

// This app consumes `react-native-rlottie` as a local ("file:..") dependency —
// its real TypeScript source lives one level up (the repo root), outside this
// app's own `node_modules`. Two things are needed for that:
//
//   1. `watchFolders` must include the repo root, or Metro (whose default
//      project root is this directory) never watches ../src, ../cpp, etc.
//   2. `react`/`react-native` (AND their subpaths, e.g. `react/jsx-runtime`,
//      which the automatic JSX transform pulls in for every file that uses
//      JSX — including ../src/RlottieView.tsx) must be forced to resolve to
//      THIS app's copies even when required from a file physically under the
//      repo root.
//
// For #2, `resolver.extraNodeModules` is NOT enough: it is only a fallback
// consulted when Metro's normal hierarchical lookup finds nothing, and a
// lookup for 'react' starting from ../src DOES find something — the repo
// root's own node_modules/react (installed there for the library's own unit
// tests, see the root CLAUDE.md, and not guaranteed to match this app's
// version). That produced two live React copies and two real, reproduced
// crashes on the Android build (`adb logcat`, ReactNativeJS tag):
//   - matching only the bare 'react'/'react-native' specifiers still let
//     'react/jsx-runtime' slip through to the repo root's copy, so JSX
//     produced by RlottieView.tsx used a DIFFERENT React than its hooks did:
//     `TypeError: Cannot read property 'ReactCurrentDispatcher' of undefined`.
//   - before that, no override at all gave: `Cannot read property 'useRef' of
//     null`.
// Matching by PACKAGE NAME (the leading path segment, so 'react/jsx-runtime'
// counts as package 'react') and redirecting the whole specifier into this
// app's own node_modules fixes both.
const repoRoot = path.resolve(__dirname, '..');

const FORCED_PACKAGES = new Set(['react', 'react-native']);

function packageNameOf(moduleName) {
  const parts = moduleName.split('/');
  return moduleName.startsWith('@') ? `${parts[0]}/${parts[1]}` : parts[0];
}

const config = {
  watchFolders: [repoRoot],
  resolver: {
    resolveRequest: (context, moduleName, platform) => {
      if (FORCED_PACKAGES.has(packageNameOf(moduleName))) {
        return context.resolveRequest(
          context,
          path.resolve(__dirname, 'node_modules', moduleName),
          platform,
        );
      }
      return context.resolveRequest(context, moduleName, platform);
    },
  },
};

module.exports = mergeConfig(getDefaultConfig(__dirname), config);
