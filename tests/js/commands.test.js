// Chunk 4.2 — tests for dispatchRlottieCommand.
//
// Run via tests/js/run-tests.sh, which compiles src/ with tsc and points the
// `react-native` import at tests/js/stubs/react-native.js. No test framework,
// matching tests/js/source.test.js and the hand-rolled C++ harness in
// tests/cpp.
//
// The properties worth protecting here (docs/bridge-contract.md, "iOS
// numeric-id fragility"):
//   * a command before/without a live native handle is dropped silently —
//     `dispatchRlottieCommand` returns `false` and never throws;
//   * a mounted view dispatches with the dynamically-resolved command id when
//     `UIManager.getViewManagerConfig` provides one, falling back to the
//     command name string otherwise;
//   * an unknown command name never throws, even though it can't resolve to a
//     real id.

const rn = require('./out/node_modules/react-native.js');
const {dispatchRlottieCommand, RlottieCommand} = require('./out/commands.js');

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

function reset() {
  rn.__uiManagerMock._config = undefined;
  rn.__dispatchedCalls.length = 0;
}

// --- Unmounted / missing handle ---------------------------------------------

reset();
ok(
  'null ref returns false, does not throw',
  dispatchRlottieCommand(null, 'play') === false,
);
ok('null ref dispatches nothing', rn.__dispatchedCalls.length === 0);

reset();
ok(
  'undefined ref returns false, does not throw',
  dispatchRlottieCommand(undefined, 'pause') === false,
);

reset();
ok(
  'component with no resolvable tag returns false',
  dispatchRlottieCommand({}, 'stop') === false,
);
ok('unresolvable-tag dispatches nothing', rn.__dispatchedCalls.length === 0);

// --- Mounted: numeric tag directly (what a raw native tag ref looks like) ---

reset();
ok(
  'numeric tag dispatches (returns true)',
  dispatchRlottieCommand(7, 'resume') === true,
);
ok('numeric tag call recorded', rn.__dispatchedCalls.length === 1);
ok('numeric tag used as reactTag', rn.__dispatchedCalls[0].reactTag === 7);

// --- Mounted: Component-like ref resolved via findNodeHandle ----------------

reset();
const fakeComponent = {__nativeTag: 99};
ok(
  'component ref dispatches (returns true)',
  dispatchRlottieCommand(fakeComponent, 'play', [10, -1]) === true,
);
ok(
  'component ref resolved to its native tag',
  rn.__dispatchedCalls[0].reactTag === 99,
);
ok(
  'args forwarded as a real array',
  Array.isArray(rn.__dispatchedCalls[0].commandArgs) &&
    rn.__dispatchedCalls[0].commandArgs.length === 2 &&
    rn.__dispatchedCalls[0].commandArgs[0] === 10 &&
    rn.__dispatchedCalls[0].commandArgs[1] === -1,
);

// --- Command id resolution: dynamic config present ---------------------------

reset();
rn.__uiManagerMock._config = {
  Commands: {
    __reservedCommandSlot0: 0,
    play: 1,
    pause: 2,
    resume: 3,
    stop: 4,
    reset: 5,
    seekToProgress: 6,
    seekToFrame: 7,
    setSpeed: 8,
  },
};
dispatchRlottieCommand(1, 'seekToFrame', [42]);
ok(
  'dynamic config id used when available',
  rn.__dispatchedCalls[0].commandId === 7,
);
ok(
  'dynamic config id matches the stable contract id',
  RlottieCommand.SeekToFrame === 7,
);

// --- Command id resolution: no config -> falls back to the name string ------

reset();
dispatchRlottieCommand(1, 'setSpeed', [2.5]);
ok(
  'falls back to the command name string when no config is available',
  rn.__dispatchedCalls[0].commandId === 'setSpeed',
);

// --- Command id resolution: config present but missing this command --------

reset();
rn.__uiManagerMock._config = {Commands: {play: 1}};
dispatchRlottieCommand(1, 'seekToProgress', [0.5]);
ok(
  'falls back to the name string when the config has no entry for this command',
  rn.__dispatchedCalls[0].commandId === 'seekToProgress',
);

// --- Unknown command name: never throws --------------------------------------

reset();
let threw = false;
let result;
try {
  // Cast through an untyped call: TS forbids this at compile time, but
  // nothing stops a plain-JS consumer (or a stale bundle) from doing it, and
  // the function's whole contract is "never throw".
  result = dispatchRlottieCommand(1, 'notARealCommand');
} catch (e) {
  threw = true;
}
ok('unknown command name does not throw', threw === false);
ok('unknown command name still dispatches (native no-ops it)', result === true);
ok(
  'unknown command name is passed through as-is',
  rn.__dispatchedCalls[0].commandId === 'notARealCommand',
);

// --- Native dispatch throwing is swallowed, not propagated ------------------

reset();
const originalDispatch = rn.__uiManagerMock.dispatchViewManagerCommand;
rn.__uiManagerMock.dispatchViewManagerCommand = () => {
  throw new Error('simulated native bridge failure');
};
let dispatchThrew = false;
let dispatchResult;
try {
  dispatchResult = dispatchRlottieCommand(1, 'play');
} catch (e) {
  dispatchThrew = true;
}
rn.__uiManagerMock.dispatchViewManagerCommand = originalDispatch;
ok(
  'a throwing native dispatch is swallowed, not propagated',
  dispatchThrew === false,
);
ok('a throwing native dispatch resolves to false', dispatchResult === false);

// --- Non-array args are defused rather than forwarded raw -------------------

reset();
dispatchRlottieCommand(1, 'play', /** @type {any} */ ('not an array'));
ok(
  'non-array args become an empty array rather than crashing native',
  Array.isArray(rn.__dispatchedCalls[0].commandArgs) &&
    rn.__dispatchedCalls[0].commandArgs.length === 0,
);

console.log(`\n${pass} passed, ${fail} failed`);
if (fail > 0) {
  process.exit(1);
}
