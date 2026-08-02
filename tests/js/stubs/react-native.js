// Stub for the `react-native` module, so the pure-logic TS in src/ can be
// exercised under plain node without a Metro bundler or a device.
//
// Keep this MINIMAL: it must only stub what the code under test actually
// touches, so a test passing here means the real code path ran, not a mock of
// it. Today that is `Image.resolveAssetSource` in src/source.ts.
module.exports = {
  Image: {
    resolveAssetSource(id) {
      // Ids the tests use as fixtures. Anything else is "unresolvable", which is
      // what RN itself returns for an id that was never registered.
      if (id === 42) return {uri: 'asset:///animations/spinner.json'};
      if (id === 7) return {uri: 'http://cdn.example.com/a.json'};
      return null;
    },
  },
};
