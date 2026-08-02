// Chunk 4.2 — tests for dispatchRlottieCommand.
// Chunk 9.9 adds the new-architecture branch's coverage at the bottom of this
// file.
//
// Run via tests/js/run-tests.sh, which compiles src/ with tsc and points the
// `react-native` import at tests/js/stubs/react-native/. No test framework,
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
//
// Chunk 9.9 adds, on the new-architecture branch: the same never-throws /
// false-for-unmounted contract, and that `setSpeed` (the public/Legacy name)
// dispatches as `setPlaybackSpeed` (the Fabric `Commands` key) — see
// docs/new-architecture-design.md §3.3.

const rn = require('./out/node_modules/react-native');
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
    playMarker: 9,
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

// --- Phase 6: playMarker (command id 9) --------------------------------------

reset();
ok(
  'RlottieCommand.PlayMarker is the frozen id 9',
  RlottieCommand.PlayMarker === 9,
);

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
    playMarker: 9,
  },
};
ok(
  'playMarker dispatches (returns true) with dynamic config',
  dispatchRlottieCommand(1, 'playMarker', ['intro']) === true,
);
ok(
  'playMarker resolves through the SAME dynamic id-resolution path as every other command',
  rn.__dispatchedCalls[0].commandId === 9,
);
ok(
  'playMarker forwards the marker name arg',
  rn.__dispatchedCalls[0].commandArgs[0] === 'intro',
);

reset();
ok(
  'playMarker falls back to the name string when no config is available, like every other command',
  dispatchRlottieCommand(1, 'playMarker', ['outro']) === true &&
    rn.__dispatchedCalls[0].commandId === 'playMarker',
);

reset();
ok(
  'playMarker on an unmounted view returns false and dispatches nothing',
  dispatchRlottieCommand(null, 'playMarker', ['intro']) === false &&
    rn.__dispatchedCalls.length === 0,
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

// =============================================================================
// Chunk 9.9 — the new-architecture dispatch branch.
//
// `rn.__setNewArchitectureEnabled(true)` sets `global.nativeFabricUIManager`,
// the same signal `src/commands.ts`'s `isNewArchitectureEnabled()` reads.
// Dispatches land in `rn.__newArchDispatchedCalls` (via the stubbed
// `codegenNativeCommands`, tests/js/stubs/react-native/Libraries/Utilities/),
// never in the Legacy `rn.__dispatchedCalls`.
// =============================================================================

function resetNewArch() {
  reset();
  rn.__newArchDispatchedCalls.length = 0;
  rn.__setNewArchitectureEnabled(true);
}

const fakeHostRef = {};

resetNewArch();
ok(
  'a mounted ref dispatches through the new-arch path (returns true)',
  dispatchRlottieCommand(fakeHostRef, 'pause') === true,
);
ok(
  'the new-arch call is recorded, not the Legacy UIManager path',
  rn.__newArchDispatchedCalls.length === 1 && rn.__dispatchedCalls.length === 0,
);
ok(
  'the ref and command name are forwarded as-is for a same-named command',
  rn.__newArchDispatchedCalls[0].ref === fakeHostRef &&
    rn.__newArchDispatchedCalls[0].command === 'pause',
);

resetNewArch();
dispatchRlottieCommand(fakeHostRef, 'play', [10, -1]);
ok(
  'play forwards its args unchanged on the new-arch path',
  rn.__newArchDispatchedCalls[0].command === 'play' &&
    rn.__newArchDispatchedCalls[0].args.length === 2 &&
    rn.__newArchDispatchedCalls[0].args[0] === 10 &&
    rn.__newArchDispatchedCalls[0].args[1] === -1,
);

// --- setSpeed -> setPlaybackSpeed: the one renamed command ------------------

resetNewArch();
dispatchRlottieCommand(fakeHostRef, 'setSpeed', [2.5]);
ok(
  'the public/Legacy "setSpeed" name dispatches as "setPlaybackSpeed" on the new-arch path',
  rn.__newArchDispatchedCalls.length === 1 &&
    rn.__newArchDispatchedCalls[0].command === 'setPlaybackSpeed',
  JSON.stringify(rn.__newArchDispatchedCalls),
);
ok(
  'the setSpeed->setPlaybackSpeed rename forwards the speed arg unchanged',
  rn.__newArchDispatchedCalls[0].args[0] === 2.5,
);

// --- Every other command name is untouched by the rename --------------------

resetNewArch();
for (const command of [
  'play',
  'pause',
  'resume',
  'stop',
  'reset',
  'seekToProgress',
  'seekToFrame',
  'playMarker',
]) {
  rn.__newArchDispatchedCalls.length = 0;
  dispatchRlottieCommand(fakeHostRef, command, []);
  ok(
    `"${command}" dispatches under its own name on the new-arch path`,
    rn.__newArchDispatchedCalls[0]?.command === command,
  );
}

// --- Unmounted / no live handle: same contract as the Legacy path ----------

resetNewArch();
ok(
  'null ref returns false on the new-arch path too',
  dispatchRlottieCommand(null, 'play') === false,
);
ok('null ref dispatches nothing', rn.__newArchDispatchedCalls.length === 0);

resetNewArch();
ok(
  'undefined ref returns false on the new-arch path too',
  dispatchRlottieCommand(undefined, 'pause') === false,
);

// --- A bare numeric tag cannot be resolved to a Fabric host instance --------

resetNewArch();
ok(
  'a raw numeric tag is not dispatchable on the new-arch path (returns false)',
  dispatchRlottieCommand(7, 'resume') === false,
);
ok(
  'a raw numeric tag under new-arch dispatches nothing on either path',
  rn.__newArchDispatchedCalls.length === 0 && rn.__dispatchedCalls.length === 0,
);

// --- A stale/unmounted ref that codegen's real dispatchCommand would throw on --

resetNewArch();
let newArchThrew = false;
let newArchResult;
try {
  newArchResult = dispatchRlottieCommand({__unmounted: true}, 'stop');
} catch (e) {
  newArchThrew = true;
}
ok(
  'a throwing new-arch dispatch is swallowed, not propagated',
  newArchThrew === false,
);
ok('a throwing new-arch dispatch resolves to false', newArchResult === false);

// --- Switching architecture back off restores the Legacy path --------------

reset();
rn.__setNewArchitectureEnabled(false);
dispatchRlottieCommand(1, 'setSpeed', [3]);
ok(
  'with the new-arch signal cleared, dispatch goes through UIManager again',
  rn.__dispatchedCalls.length === 1 &&
    rn.__dispatchedCalls[0].commandId === 'setSpeed' &&
    rn.__newArchDispatchedCalls.length === 0,
);

console.log(`\n${pass} passed, ${fail} failed`);
if (fail > 0) {
  process.exit(1);
}
