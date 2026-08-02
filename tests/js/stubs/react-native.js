// Stub for the `react-native` module, so the pure-logic TS in src/ can be
// exercised under plain node without a Metro bundler or a device.
//
// Keep this MINIMAL: it must only stub what the code under test actually
// touches, so a test passing here means the real code path ran, not a mock of
// it. Today that is `Image.resolveAssetSource` in src/source.ts, plus
// `UIManager`/`findNodeHandle`/`requireNativeComponent` for src/commands.ts
// and src/RlottieNativeComponent.ts (Chunk 4.2).
//
// The UIManager/findNodeHandle shapes below are captured from the real
// implementations in node_modules/react-native@0.81 (UIManager.js,
// PaperUIManager.js, RCTUIManager.mm's dispatchViewManagerCommand) — see
// src/commands.ts's `resolveCommandId` doc comment for the full reasoning.
// Tests can reach into `module.exports.__uiManagerMock` to configure/inspect
// call behaviour.
const dispatchedCalls = [];

const uiManagerMock = {
  // Populated per-test via __setViewManagerConfig; PaperUIManager.js computes
  // this dynamically from the native manager's real exported methods, so the
  // stub mirrors that shape: { Commands: { [name]: number } }.
  _config: undefined,

  getViewManagerConfig(name) {
    if (name !== 'RlottieView') {
      return null;
    }
    return uiManagerMock._config;
  },

  dispatchViewManagerCommand(reactTag, commandId, commandArgs) {
    if (reactTag == null) {
      throw new Error('dispatchViewManagerCommand: found null reactTag');
    }
    dispatchedCalls.push({reactTag, commandId, commandArgs});
  },
};

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

  UIManager: uiManagerMock,

  findNodeHandle(componentOrHandle) {
    if (componentOrHandle == null) {
      return null;
    }
    if (typeof componentOrHandle === 'number') {
      return componentOrHandle;
    }
    // Mirrors the real renderer: a mounted host/composite instance exposes
    // its native tag; anything else resolves to null rather than throwing.
    if (
      typeof componentOrHandle === 'object' &&
      typeof componentOrHandle.__nativeTag === 'number'
    ) {
      return componentOrHandle.__nativeTag;
    }
    return null;
  },

  requireNativeComponent(name) {
    // src/RlottieNativeComponent.ts calls this at module-load time; return an
    // opaque marker rather than a real host component, since no test renders
    // through react-test-renderer here.
    return {displayName: name};
  },

  // Test-only escape hatches, not part of the real react-native surface.
  __uiManagerMock: uiManagerMock,
  __dispatchedCalls: dispatchedCalls,
};
