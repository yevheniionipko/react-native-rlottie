// Chunk 4.1 — tests for source normalization + cache-key derivation.
//
// Run via tests/js/run-tests.sh, which compiles src/ with tsc and points the
// `react-native` import at tests/js/stubs/. No test framework, matching the
// hand-rolled C++ harness in tests/cpp — this package ships zero dependencies
// and the test tooling follows the same rule.
//
// The properties worth protecting here (Chunk 5.3 will touch this code):
//   * an object source is stringified EXACTLY once (plan §12);
//   * two different payloads can never share a cache key, whatever the caller
//     passes as `cacheKey` (plan §15);
//   * an invalid source RETURNS a typed failure and never throws — throwing
//     would take out a React render.

const crypto = require('crypto');

const {normalizeSource, isSameNormalizedSource} = require('./out/source.js');
const {sha256Hex} = require('./out/sha256.js');

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

// --- SHA-256 correctness, against node:crypto -------------------------------
// The cache key is only as trustworthy as the digest under it.

ok(
  'sha256 empty',
  sha256Hex('') === crypto.createHash('sha256').update('').digest('hex'),
);
ok(
  'sha256 abc',
  sha256Hex('abc') === crypto.createHash('sha256').update('abc').digest('hex'),
);

for (let i = 0; i < 130; i++) {
  // Sweeps the 55/56/63/64/119/120-byte padding boundaries, where a
  // hand-written SHA-256 typically breaks.
  const s = 'a'.repeat(i);
  ok(
    `sha256 len ${i}`,
    sha256Hex(s) ===
      crypto.createHash('sha256').update(s, 'utf8').digest('hex'),
  );
}

for (const [label, s] of [
  ['2-byte', 'héllo wörld'],
  ['3-byte', '日本語のアニメーション'],
  ['4-byte surrogate pair', '🎬🎞️ lottie 🚀'],
  ['lone high surrogate', 'a\uD800b'],
  ['lone low surrogate', 'a\uDC00b'],
  ['multi-block 64KB', 'x'.repeat(65536)],
]) {
  ok(
    `sha256 utf8 ${label}`,
    sha256Hex(s) ===
      crypto.createHash('sha256').update(s, 'utf8').digest('hex'),
  );
}

// --- Accepted sources -------------------------------------------------------

const obj = {v: '5.7.4', layers: [{ty: 4}]};
const first = normalizeSource({json: obj});
ok('object json accepted', first.ok && typeof first.source.json === 'string');

// The memoization guarantee: same object identity => same result object, which
// means JSON.stringify + sha256 ran only once.
ok(
  'object source memoized on identity',
  normalizeSource({json: obj}) === first,
);

const equalContent = normalizeSource({json: {v: '5.7.4', layers: [{ty: 4}]}});
ok(
  'equal content produces an equal cacheKey',
  equalContent.ok && equalContent.source.cacheKey === first.source.cacheKey,
);

const differentContent = normalizeSource({
  json: {v: '5.7.4', layers: [{ty: 5}]},
});
ok(
  'different content produces a different cacheKey',
  differentContent.source.cacheKey !== first.source.cacheKey,
);

// plan §15: "Never let two different JSON payloads share a caller-provided
// cache key silently." The caller's label must not be able to force a collision.
const sameLabelA = normalizeSource({json: '{"a":1}', cacheKey: 'shared'});
const sameLabelB = normalizeSource({json: '{"a":2}', cacheKey: 'shared'});
ok(
  'a caller cacheKey cannot alias two different payloads',
  sameLabelA.source.cacheKey !== sameLabelB.source.cacheKey,
);

const otherLabel = normalizeSource({json: '{"a":1}', cacheKey: 'other'});
ok(
  'identical bytes under different labels stay distinguishable',
  otherLabel.source.cacheKey !== sameLabelA.source.cacheKey,
);

ok(
  'cacheKey is sha256(json):callerKey',
  sameLabelA.source.cacheKey === `${sha256Hex('{"a":1}')}:shared`,
);

const withResource = normalizeSource({
  json: '{"a":1}',
  resourcePath: '/data/app/assets',
});
ok(
  'resourcePath preserved',
  withResource.ok && withResource.source.resourcePath === '/data/app/assets',
);
ok('resourcePath omitted when unset', !('resourcePath' in sameLabelA.source));

for (const uri of [
  'file:///tmp/a.json',
  'asset:///anim/a.json',
  'content://media/external/file/1',
  'FILE:///Case.json', // scheme comparison is case-insensitive
]) {
  ok(`uri accepted: ${uri}`, normalizeSource({uri}).ok);
}

ok(
  'path accepted',
  normalizeSource({path: '/data/user/0/app/files/a.json'}).ok,
);

const assetResult = normalizeSource(42);
ok(
  'require()d asset id resolves through the asset registry',
  assetResult.ok &&
    assetResult.source.uri === 'asset:///animations/spinner.json',
);

// --- Rejected sources: typed failure, never a throw -------------------------

const circular = {a: 1};
circular.self = circular;

for (const [label, input] of [
  ['https url (v1.1 scope)', {uri: 'https://cdn.example.com/a.json'}],
  ['http url (v1.1 scope)', {uri: 'http://cdn.example.com/a.json'}],
  ['unknown scheme', {uri: 'ftp://host/a.json'}],
  ['bare path in uri', {uri: '/tmp/a.json'}],
  ['empty uri', {uri: ''}],
  ['empty json string', {json: ''}],
  ['json of the wrong type', {json: 123}],
  ['object with no recognized key', {}],
  ['null', null],
  ['undefined', undefined],
  ['a bare string', 'anim.json'],
  ['unregistered asset id', 99],
  ['asset id resolving to http', 7],
  ['circular object', {json: circular}],
]) {
  let result;
  try {
    result = normalizeSource(input);
  } catch (e) {
    fail++;
    console.log(`  FAIL: ${label} THREW instead of returning a failure: ${e}`);
    continue;
  }
  ok(
    `rejected with INVALID_SOURCE: ${label}`,
    result.ok === false && result.code === 'INVALID_SOURCE',
    JSON.stringify(result),
  );
}

// --- Remote loading gate (Chunk 5.4) -----------------------------------------
// The gate changes REJECTION WORDING only — every case here still fails with
// INVALID_SOURCE, flag or no flag, since v1 never performs network I/O.

const httpsNoFlag = normalizeSource({uri: 'https://cdn.example.com/a.json'});
const httpsWithFlag = normalizeSource(
  {uri: 'https://cdn.example.com/a.json'},
  true,
);
const httpNoFlag = normalizeSource({uri: 'http://cdn.example.com/a.json'});
const httpWithFlag = normalizeSource(
  {uri: 'http://cdn.example.com/a.json'},
  true,
);

ok(
  'https without allowRemoteSources is rejected',
  httpsNoFlag.ok === false && httpsNoFlag.code === 'INVALID_SOURCE',
);
ok(
  'https with allowRemoteSources is still rejected (not implemented, not malformed)',
  httpsWithFlag.ok === false && httpsWithFlag.code === 'INVALID_SOURCE',
);
ok(
  'http is rejected without allowRemoteSources',
  httpNoFlag.ok === false && httpNoFlag.code === 'INVALID_SOURCE',
);
ok(
  'http is rejected even WITH allowRemoteSources (HTTPS-only, plan §16)',
  httpWithFlag.ok === false && httpWithFlag.code === 'INVALID_SOURCE',
);

// The messages must differ across states — a developer who has already opted
// in should not be told their https URL "isn't supported"/looks malformed;
// they should be told remote loading itself isn't implemented yet.
ok(
  'https message differs with vs. without the flag',
  httpsNoFlag.message !== httpsWithFlag.message,
);
ok(
  'https-without-flag message points at allowRemoteSources',
  httpsNoFlag.message.includes('allowRemoteSources'),
);
ok(
  'https-with-flag message says remote loading is not implemented, not that the url is invalid',
  /not implemented/i.test(httpsWithFlag.message) &&
    !/scheme/i.test(httpsWithFlag.message),
);
ok(
  'http message (without flag) explains HTTPS-only, distinct from the https message',
  httpNoFlag.message !== httpsNoFlag.message &&
    /https/i.test(httpNoFlag.message),
);
ok(
  'http message stays the same shape with the flag set (still HTTPS-only)',
  httpWithFlag.message !== httpsWithFlag.message &&
    /https/i.test(httpWithFlag.message),
);

// --- isSameNormalizedSource -------------------------------------------------

ok(
  'same content compares equal',
  isSameNormalizedSource(first.source, equalContent.source),
);
ok(
  'different content compares unequal',
  !isSameNormalizedSource(first.source, differentContent.source),
);
ok(
  'undefined pair compares equal',
  isSameNormalizedSource(undefined, undefined),
);
ok(
  'one-sided undefined compares unequal',
  !isSameNormalizedSource(first.source, undefined),
);
ok(
  'differing resourcePath compares unequal',
  !isSameNormalizedSource(
    normalizeSource({json: '{"a":1}'}).source,
    normalizeSource({json: '{"a":1}', resourcePath: '/x'}).source,
  ),
);

console.log(`\n${pass} passed, ${fail} failed`);
if (fail > 0) {
  process.exit(1);
}
