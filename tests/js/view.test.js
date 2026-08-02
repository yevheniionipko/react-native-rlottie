// Chunk 8.1 — tests for RlottieView's RENDER-TIME behaviour.
//
// This closes the gap CLAUDE.md recorded through Phase 7: everything else in
// src/ had tests, but the component that consumers actually mount did not, and
// its two subtlest behaviours are exactly the kind that regress silently
// because nothing crashes when they break.
//
//   1. A source-normalization failure is reported as `onAnimationError` from an
//      EFFECT (never during render), deduped on the FAILURE CONTENT rather than
//      on the `source` object's identity. Consumers write inline
//      `source={{uri}}` literals — a fresh object every render — so an identity
//      guard would re-fire forever, and any consumer that calls setState in
//      that handler would spin.
//   2. The normalized source is handed to native at a STABLE object identity
//      while its content is unchanged. Handing a fresh object down would look
//      like a changed prop, re-issue a native load, bump the generation, and
//      restart the animation on every unrelated re-render.
//
// Both are invisible to a type checker and to every other test here, and both
// are "works, but janks/loops on a real device" failures.
//
// Run via tests/js/run-tests.sh. Uses react-test-renderer (already a
// devDependency) rather than a new framework, and the same `react-native` stub
// as the other tests — so the real normalizeSource/dispatch code runs, not a
// mock of it. `createNodeMock` supplies the `__nativeTag` the stub's
// findNodeHandle looks for, so ref commands travel the real dispatch path.

// React only enables act() (and stops warning about it) when this global is
// set. It is normally set by a test framework's environment; there is no
// framework here, so set it explicitly before requiring the renderer.
global.IS_REACT_ACT_ENVIRONMENT = true;

const React = require('react');
// react-test-renderer prints its own deprecation notice on React 19. It is
// still what React Native's own test tooling uses for host-component trees, and
// it is the only renderer that can inspect native props without a DOM — so the
// notice is expected noise here, not a problem to fix by swapping renderers.
const TestRenderer = require('react-test-renderer');

const rn = require('./out/node_modules/react-native');
const {RlottieView} = require('./out/RlottieView.js');

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

const NATIVE_TAG = 11;

function render(element) {
  let renderer;
  TestRenderer.act(() => {
    renderer = TestRenderer.create(element, {
      createNodeMock: () => ({__nativeTag: NATIVE_TAG}),
    });
  });
  return renderer;
}

function update(renderer, element) {
  TestRenderer.act(() => {
    renderer.update(element);
  });
}

// The single host element RlottieView renders. Its props are what actually
// crosses the bridge.
function nativeProps(renderer) {
  return renderer.root.findByType('RlottieView').props;
}

const VALID_URI = 'file:///data/user/0/app/files/a.json';
const OTHER_URI = 'file:///data/user/0/app/files/b.json';
// No scheme -> INVALID_SOURCE (src/source.ts normalizeUri).
const BAD_URI = 'no-scheme-here.json';
const OTHER_BAD_URI = 'http://cdn.example.com/a.json'; // a DIFFERENT failure message

// --- Defaults reach native as concrete values -------------------------------
//
// docs/bridge-contract.md's documented defaults live in RlottieView, so a drift
// between the doc and what native receives shows up here.

{
  const r = render(
    React.createElement(RlottieView, {source: {uri: VALID_URI}}),
  );
  const p = nativeProps(r);
  ok('default autoPlay is false', p.autoPlay === false);
  ok('default loop is false', p.loop === false);
  ok('default repeatCount is 0', p.repeatCount === 0);
  ok('default speed is 1', p.speed === 1);
  ok('default startFrame is 0', p.startFrame === 0);
  ok('default endFrame is 0', p.endFrame === 0);
  ok("default resizeMode is 'contain'", p.resizeMode === 'contain');
  ok('default renderScale is 1', p.renderScale === 1);
  ok('default pauseWhenInactive is true', p.pauseWhenInactive === true);
  ok("default cacheStrategy is 'model'", p.cacheStrategy === 'model');
  ok('default metricsEnabled is false', p.metricsEnabled === false);
  ok(
    'progress stays undefined when not provided (native keeps its own clock)',
    p.progress === undefined,
  );
}

// --- Value sanitizing before the bridge -------------------------------------

{
  const r = render(
    React.createElement(RlottieView, {
      source: {uri: VALID_URI},
      speed: NaN,
      progress: 4,
    }),
  );
  ok('non-finite speed falls back to 1', nativeProps(r).speed === 1);
  ok('progress > 1 is clamped to 1', nativeProps(r).progress === 1);

  update(
    r,
    React.createElement(RlottieView, {
      source: {uri: VALID_URI},
      progress: -3,
    }),
  );
  ok('progress < 0 is clamped to 0', nativeProps(r).progress === 0);

  update(
    r,
    React.createElement(RlottieView, {
      source: {uri: VALID_URI},
      progress: NaN,
    }),
  );
  ok('non-finite progress clamps to 0', nativeProps(r).progress === 0);
}

// --- Error reporting: once per distinct failure, never during render ---------

{
  const calls = [];
  const onAnimationError = e => calls.push(e.nativeEvent);

  const r = render(
    React.createElement(RlottieView, {
      source: {uri: BAD_URI},
      onAnimationError,
    }),
  );
  ok('a bad source reports onAnimationError once', calls.length === 1);
  ok(
    'the reported code is INVALID_SOURCE',
    calls.length === 1 && calls[0].code === 'INVALID_SOURCE',
    calls.length === 1 ? JSON.stringify(calls[0]) : undefined,
  );

  // Three re-renders with a FRESH object literal each time, exactly as a
  // consumer writing `source={{uri}}` inline produces. An identity-keyed guard
  // would fire three more times here.
  for (let i = 0; i < 3; i++) {
    update(
      r,
      React.createElement(RlottieView, {
        source: {uri: BAD_URI},
        onAnimationError,
      }),
    );
  }
  ok(
    'an unchanged failure does not re-fire across fresh inline source objects',
    calls.length === 1,
    `fired ${calls.length} time(s)`,
  );

  // A DIFFERENT failure is a new fact and must be reported.
  update(
    r,
    React.createElement(RlottieView, {
      source: {uri: OTHER_BAD_URI},
      onAnimationError,
    }),
  );
  ok('a distinct failure is reported', calls.length === 2);
  ok(
    'the two failures carry different messages',
    calls.length === 2 && calls[0].message !== calls[1].message,
  );

  // Recovering and failing again the SAME way must report again — the dedup is
  // "already reported THIS failure", not "reported once, ever".
  update(
    r,
    React.createElement(RlottieView, {
      source: {uri: VALID_URI},
      onAnimationError,
    }),
  );
  ok('recovery reports nothing', calls.length === 2);
  update(
    r,
    React.createElement(RlottieView, {
      source: {uri: OTHER_BAD_URI},
      onAnimationError,
    }),
  );
  ok('the same failure after a recovery is reported again', calls.length === 3);
}

// A failing source must not leave the last good one on the native prop: both
// resolvers treat a null source as "no change", so the view keeps rendering
// what it has instead of blanking or silently reloading a stale source.
{
  const r = render(
    React.createElement(RlottieView, {source: {uri: VALID_URI}}),
  );
  ok('a good source reaches native', nativeProps(r).source !== undefined);
  update(r, React.createElement(RlottieView, {source: {uri: BAD_URI}}));
  ok(
    'a failing source sends undefined rather than the last good value',
    nativeProps(r).source === undefined,
  );
}

// --- Native source prop identity is stable while content is unchanged -------

{
  const r = render(
    React.createElement(RlottieView, {source: {uri: VALID_URI}}),
  );
  const first = nativeProps(r).source;

  update(r, React.createElement(RlottieView, {source: {uri: VALID_URI}}));
  ok(
    'a content-equal inline source keeps the SAME normalized object identity',
    nativeProps(r).source === first,
  );

  // An unrelated re-render (a changed prop that has nothing to do with source)
  // must not disturb it either.
  update(
    r,
    React.createElement(RlottieView, {source: {uri: VALID_URI}, loop: true}),
  );
  ok(
    'an unrelated prop change does not re-issue the source',
    nativeProps(r).source === first,
  );

  update(r, React.createElement(RlottieView, {source: {uri: OTHER_URI}}));
  const second = nativeProps(r).source;
  ok('a genuinely different source produces a new object', second !== first);
  ok(
    'and the new object carries the new uri',
    second !== undefined && second.uri === OTHER_URI,
  );
}

// Inline object (JSON) sources take the same path, and are the case where a
// re-normalization would also re-hash on the JS thread (Chunk 4.1).
{
  const json = {v: '5.5.7', w: 8, h: 8, ip: 0, op: 1, fr: 30, layers: []};
  const r = render(
    React.createElement(RlottieView, {source: {json: {...json}}}),
  );
  const first = nativeProps(r).source;
  ok('an inline json source normalizes', first !== undefined);
  update(r, React.createElement(RlottieView, {source: {json: {...json}}}));
  ok(
    'a content-equal inline json object keeps the same identity',
    nativeProps(r).source === first,
  );
}

// --- Imperative ref commands travel the real dispatch path ------------------

function withRef(extraProps) {
  rn.__uiManagerMock._config = {
    Commands: {
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
  rn.__dispatchedCalls.length = 0;
  const ref = React.createRef();
  const r = render(
    React.createElement(RlottieView, {
      source: {uri: VALID_URI},
      ref,
      ...extraProps,
    }),
  );
  return {ref, renderer: r};
}

{
  const {ref} = withRef();
  TestRenderer.act(() => ref.current.play());
  const call = rn.__dispatchedCalls[0];
  ok('play dispatches the resolved command id', call && call.commandId === 1);
  ok(
    'an argument-less play sends the -1/-1 "unset" sentinels, not 0/0',
    call && call.commandArgs[0] === -1 && call.commandArgs[1] === -1,
    call ? JSON.stringify(call.commandArgs) : 'no call',
  );
  ok(
    'the dispatch uses the resolved native tag',
    call && call.reactTag === NATIVE_TAG,
  );
}

{
  const {ref} = withRef();
  TestRenderer.act(() => ref.current.play({startFrame: 2, endFrame: 5}));
  ok(
    'play forwards an explicit frame range',
    rn.__dispatchedCalls[0].commandArgs[0] === 2 &&
      rn.__dispatchedCalls[0].commandArgs[1] === 5,
  );
}

{
  const {ref} = withRef();
  // A marker WINS over a frame range rather than merging (contract, command 9).
  TestRenderer.act(() =>
    ref.current.play({marker: 'mid', startFrame: 2, endFrame: 5}),
  );
  const call = rn.__dispatchedCalls[0];
  ok('play({marker}) dispatches playMarker', call && call.commandId === 9);
  ok(
    'playMarker sends only the marker name — the frame range is dropped, not merged',
    call && call.commandArgs.length === 1 && call.commandArgs[0] === 'mid',
    call ? JSON.stringify(call.commandArgs) : 'no call',
  );
}

{
  const {ref} = withRef();
  TestRenderer.act(() => {
    ref.current.pause();
    ref.current.resume();
    ref.current.stop();
    ref.current.reset();
  });
  const ids = rn.__dispatchedCalls.map(c => c.commandId);
  ok(
    'pause/resume/stop/reset map to their own ids in order',
    ids.join(',') === '2,3,4,5',
    ids.join(','),
  );
}

{
  const {ref} = withRef();
  TestRenderer.act(() => {
    ref.current.seekToProgress(2.5);
    ref.current.seekToProgress(-1);
    ref.current.seekToFrame(7);
    ref.current.setSpeed(NaN);
  });
  const args = rn.__dispatchedCalls.map(c => c.commandArgs[0]);
  ok('seekToProgress clamps above 1', args[0] === 1);
  ok('seekToProgress clamps below 0', args[1] === 0);
  ok('seekToFrame passes the frame through', args[2] === 7);
  ok('setSpeed falls back to 1 on a non-finite value', args[3] === 1);
}

// A command issued after unmount must fail safe, not throw — `ref.play()`
// racing teardown is ordinary consumer code.
//
// Note WHICH object is called here: React nulls `ref.current` on unmount, so
// `ref.current.play()` would throw on `null` before reaching any of our code
// and would prove nothing. The library's actual guarantee is in
// dispatchRlottieCommand, which must no-op (return false) once the view has no
// native handle — so the test holds the imperative API object captured while
// mounted and calls it afterwards, which is also what a consumer's
// `const {play} = ref.current` or a captured callback does.
{
  const {ref, renderer} = withRef();
  const api = ref.current;
  TestRenderer.act(() => renderer.unmount());
  rn.__dispatchedCalls.length = 0;
  let threw = false;
  try {
    api.play();
    api.pause();
    api.seekToProgress(0.5);
  } catch {
    threw = true;
  }
  ok('a command after unmount does not throw', threw === false);
  ok(
    'and dispatches nothing to a view that no longer exists',
    rn.__dispatchedCalls.length === 0,
    `${rn.__dispatchedCalls.length} dispatch(es)`,
  );
  ok('React itself nulls ref.current on unmount', ref.current === null);
}

// --- Native events pass through to the consumer's handlers ------------------

{
  const seen = [];
  const r = render(
    React.createElement(RlottieView, {
      source: {uri: VALID_URI},
      onAnimationLoaded: () => seen.push('loaded'),
      onAnimationStart: () => seen.push('start'),
      onAnimationPause: () => seen.push('pause'),
      onAnimationLoop: () => seen.push('loop'),
      onAnimationFinish: () => seen.push('finish'),
      onMetrics: () => seen.push('metrics'),
    }),
  );
  const p = nativeProps(r);
  TestRenderer.act(() => {
    p.onAnimationLoaded({nativeEvent: {}});
    p.onAnimationStart({nativeEvent: {}});
    p.onAnimationPause({nativeEvent: {}});
    p.onAnimationLoop({nativeEvent: {}});
    p.onAnimationFinish({nativeEvent: {}});
    p.onMetrics({nativeEvent: {}});
  });
  ok(
    'all six native events reach their consumer handler',
    seen.join(',') === 'loaded,start,pause,loop,finish,metrics',
    seen.join(','),
  );
}

// Every event handler prop is always passed to native, even when the consumer
// supplied none — the native side decides whether to emit, and a conditionally
// absent prop would make that a per-render decision on the JS side.
{
  const r = render(
    React.createElement(RlottieView, {source: {uri: VALID_URI}}),
  );
  const p = nativeProps(r);
  ok(
    'event props are present even with no consumer handlers',
    typeof p.onAnimationLoaded === 'function' &&
      typeof p.onAnimationError === 'function' &&
      typeof p.onMetrics === 'function',
  );
}

console.log(`\n${pass} passed, ${fail} failed`);
if (fail > 0) {
  process.exit(1);
}
