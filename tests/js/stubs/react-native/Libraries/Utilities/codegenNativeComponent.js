// Stub for react-native/Libraries/Utilities/codegenNativeComponent.js (Flow
// source, not requireable directly under plain node — see the note in
// tests/js/stubs/react-native/index.js).
//
// Returns the name string, same as this package's `requireNativeComponent`
// stub (index.js) — a valid host element type for react-test-renderer, and
// what `tests/js/view.test.js` matches on via `findByType('RlottieView')`.
function codegenNativeComponent(name) {
  return name;
}

module.exports = codegenNativeComponent;
