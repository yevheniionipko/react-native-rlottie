// Chunk 4.4 — tests for the global `Rlottie` module and the package entry point.
//
// Run via tests/js/run-tests.sh. See tests/js/source.test.js for why there is no
// test framework here.
//
// The properties worth protecting:
//   * every method forwards to the native module and RETURNS its promise — a
//     dropped `return` would make `await Rlottie.configure(...)` resolve before
//     the native work finished, on both platforms, silently;
//   * a missing native module fails at the CALL with a diagnostic, not at
//     import — importing the package must not explode in a JS-only environment;
//   * the entry point exports the documented public surface and nothing that
//     would let a consumer bypass the command-dispatch guards.

// Require the STUB by path, not as a bare specifier: from this directory a bare
// `require('react-native')` resolves to the real package in the repo's
// node_modules, which is Flow-typed source that node cannot parse. The compiled
// code under ./out resolves it via ./out/node_modules, so this is the same
// module instance the code under test sees.
const rn = require('./out/node_modules/react-native.js');
const {
  Rlottie,
  configure,
  clearModelCache,
  getNativeVersion,
  isAvailable,
} = require('./out/RlottieModule.js');

let pass = 0;
let fail = 0;

function ok(label, cond, extra) {
  if (cond) {
    pass++;
  } else {
    fail++;
    console.log(`  FAIL: ${label}${extra ? `\n        ${extra}` : ''}`);
  }
}

async function main() {
  const calls = rn.__nativeModuleCalls;

  // --- forwarding + promise plumbing ---------------------------------------

  calls.length = 0;
  const configureResult = configure({modelCacheSize: 16});
  ok(
    'configure returns a promise',
    typeof configureResult?.then === 'function',
  );
  await configureResult;
  ok(
    'configure forwards to the native module with its options',
    calls.length === 1 &&
      calls[0].method === 'configure' &&
      calls[0].args[0].modelCacheSize === 16,
    JSON.stringify(calls),
  );

  calls.length = 0;
  await clearModelCache();
  ok(
    'clearModelCache forwards to the native module',
    calls.length === 1 && calls[0].method === 'clearModelCache',
    JSON.stringify(calls),
  );

  calls.length = 0;
  const version = await getNativeVersion();
  ok(
    'getNativeVersion resolves the native payload',
    version.rlottieCommit === 'deadbeef' && version.modelCacheSize === 10,
    JSON.stringify(version),
  );

  // A null/undefined options object must not become `undefined` on the bridge.
  calls.length = 0;
  await configure(undefined);
  ok(
    'configure(undefined) forwards an object, not undefined',
    calls.length === 1 &&
      typeof calls[0].args[0] === 'object' &&
      calls[0].args[0] !== null,
    JSON.stringify(calls),
  );

  ok('isAvailable() is true when linked', isAvailable() === true);

  // The namespace object and the named exports are the same functions.
  ok('Rlottie.configure === configure', Rlottie.configure === configure);
  ok(
    'Rlottie exposes exactly the documented methods',
    ['configure', 'clearModelCache', 'getNativeVersion', 'isAvailable'].every(
      k => typeof Rlottie[k] === 'function',
    ) && Object.keys(Rlottie).length === 4,
    Object.keys(Rlottie).join(','),
  );

  // --- the entry point ------------------------------------------------------

  const pkg = require('./out/index.js');
  for (const name of [
    'RlottieView',
    'default',
    'Rlottie',
    'configure',
    'clearModelCache',
    'getNativeVersion',
    'isAvailable',
    'RlottieCommand',
  ]) {
    ok(`index exports ${name}`, pkg[name] !== undefined);
  }
  ok('index default export is RlottieView', pkg.default === pkg.RlottieView);
  ok(
    'RlottieCommand ids match the frozen contract',
    pkg.RlottieCommand.Play === 1 &&
      pkg.RlottieCommand.Pause === 2 &&
      pkg.RlottieCommand.Resume === 3 &&
      pkg.RlottieCommand.Stop === 4 &&
      pkg.RlottieCommand.Reset === 5 &&
      pkg.RlottieCommand.SeekToProgress === 6 &&
      pkg.RlottieCommand.SeekToFrame === 7 &&
      pkg.RlottieCommand.SetSpeed === 8,
  );
  // The dispatcher is deliberately NOT part of the public surface: a consumer
  // dispatching by hand would bypass the unmounted-view guard and the
  // id-resolution that avoids the iOS raw-index path.
  ok(
    'index does not export the raw dispatcher',
    pkg.dispatchRlottieCommand === undefined,
  );

  // --- missing native module ------------------------------------------------
  //
  // Re-require the module with NativeModules.RlottieModule removed, to prove the
  // failure lands at the call site with a diagnostic rather than at import.

  delete require.cache[require.resolve('./out/RlottieModule.js')];
  const savedModule = rn.__nativeModules.RlottieModule;
  delete rn.__nativeModules.RlottieModule;

  let unlinked;
  try {
    unlinked = require('./out/RlottieModule.js');
    ok('importing without the native module does not throw', true);
  } catch (e) {
    ok('importing without the native module does not throw', false, String(e));
  }

  if (unlinked) {
    ok(
      'isAvailable() is false when unlinked',
      unlinked.isAvailable() === false,
    );
    let threw;
    try {
      unlinked.clearModelCache();
    } catch (e) {
      threw = e;
    }
    ok('calling without the native module throws', threw !== undefined);
    ok(
      'the error names the module and says how to fix it',
      threw !== undefined &&
        /RlottieModule/.test(threw.message) &&
        /pod install/.test(threw.message),
      threw && threw.message,
    );
  }

  // Restore, so ordering between test files can never matter.
  rn.__nativeModules.RlottieModule = savedModule;
  delete require.cache[require.resolve('./out/RlottieModule.js')];

  console.log(`\n${pass} passed, ${fail} failed`);
  if (fail > 0) {
    process.exit(1);
  }
}

main().catch(e => {
  console.error(e);
  process.exit(1);
});
