// Stub for the `react-native` module, so the pure-logic TS in src/ can be
// exercised under plain node without a Metro bundler or a device.
//
// Keep this MINIMAL: it must only stub what the code under test actually
// touches, so a test passing here means the real code path ran, not a mock of
// it. Today that is `Image.resolveAssetSource` in src/source.ts, plus
// `UIManager`/`findNodeHandle`/`requireNativeComponent` for src/commands.ts
// and src/RlottieNativeComponent.ts (Chunk 4.2), plus `TurboModuleRegistry`
// (src/specs/NativeRlottieModule.ts) and the deep `codegenNativeCommands`/
// `codegenNativeComponent` imports in `src/specs/RlottieViewNativeComponent.ts`
// (Chunk 9.9 — those stubs live in `Libraries/Utilities/` below, a real
// directory under this package so the deep-import path resolves here instead
// of falling through to the real (Flow-only, not Node-parseable)
// `node_modules/react-native`).
//
// The UIManager/findNodeHandle shapes below are captured from the real
// implementations in node_modules/react-native@0.81 (UIManager.js,
// PaperUIManager.js, RCTUIManager.mm's dispatchViewManagerCommand) — see
// src/commands.ts's `resolveCommandId` doc comment for the full reasoning.
// Tests can reach into `module.exports.__uiManagerMock` to configure/inspect
// call behaviour.
const dispatchedCalls = [];

// What `Libraries/Utilities/codegenNativeCommands.js` (stubbed alongside this
// file) records when the new-architecture dispatch path runs.
const newArchDispatchedCalls = [];

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

// Records what src/RlottieModule.ts forwards to the native module. The three
// methods are Promise-based on both platforms (docs/bridge-contract.md), so the
// stub returns promises too — a stub that resolved synchronously would let a
// missing `await` pass here and fail on a device.
const nativeModuleCalls = [];

const rlottieNativeModule = {
  configure(options) {
    nativeModuleCalls.push({method: 'configure', args: [options]});
    return Promise.resolve();
  },
  clearModelCache() {
    nativeModuleCalls.push({method: 'clearModelCache', args: []});
    return Promise.resolve();
  },
  getNativeVersion() {
    nativeModuleCalls.push({method: 'getNativeVersion', args: []});
    return Promise.resolve({rlottieCommit: 'deadbeef', modelCacheSize: 10});
  },
};

// Mutable so a test can simulate the "native module not linked" case, which is
// the branch consumers hit when they forget `pod install` or skip a rebuild.
const nativeModules = {RlottieModule: rlottieNativeModule};

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

  NativeModules: nativeModules,

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
    // src/RlottieNativeComponent.ts calls this at module-load time. Returning
    // the NAME STRING (not an opaque marker object) makes the result a valid
    // host element type, which is what lets tests/js/view.test.js render
    // RlottieView through react-test-renderer and read the props that actually
    // cross the bridge via `findByType('RlottieView')`. Under a real renderer a
    // host component is exactly this: a type identified by its name.
    return name;
  },

  // src/RlottieModule.ts (Chunk 9.9) resolves through
  // `specs/NativeRlottieModule.ts`'s `TurboModuleRegistry.get('RlottieModule')`
  // rather than `NativeModules['RlottieModule']` directly. The real
  // `TurboModuleRegistry.get` falls back to `NativeModules` on the old
  // architecture (docs/new-architecture-design.md §2.2) — mirrored here as a
  // direct `NativeModules` lookup, since this stub has no Fabric/TurboModule
  // machinery to fall back FROM.
  TurboModuleRegistry: {
    get(name) {
      return nativeModules[name] ?? null;
    },
  },

  // Test-only escape hatches, not part of the real react-native surface.
  __uiManagerMock: uiManagerMock,
  __dispatchedCalls: dispatchedCalls,
  __newArchDispatchedCalls: newArchDispatchedCalls,
  __nativeModuleCalls: nativeModuleCalls,
  __nativeModules: nativeModules,

  // Flips the same signal src/commands.ts's `isNewArchitectureEnabled` reads.
  __setNewArchitectureEnabled(enabled) {
    if (enabled) {
      global.nativeFabricUIManager = {};
    } else {
      delete global.nativeFabricUIManager;
    }
  },
};
