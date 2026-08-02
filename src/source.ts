// Chunk 4.1 — source normalization and deterministic cache keys.
//
// This is the ONE place a JS `source` prop becomes the shape the native
// resolvers accept (docs/bridge-contract.md). Two rules drive the design:
//
//   1. Stringify an object source EXACTLY ONCE (plan §12). RN re-runs render
//      freely, and `JSON.stringify` on a multi-megabyte animation — plus the
//      SHA-256 that follows it — is far too expensive to repeat per render, so
//      results are memoized on the source object's identity.
//   2. Never let two different payloads silently share a cache key (plan §15).
//      The key is derived from CONTENT, not from a caller-supplied label.
//
// Normalization never throws. An invalid source returns a typed failure so the
// view can surface it as an `onAnimationError` with a code from
// `cpp/ErrorCode.h`; throwing here would blow up a render instead.

import {Image} from 'react-native';

import {sha256Hex} from './sha256';
import type {
  NormalizedRlottieSource,
  RlottieErrorCode,
  RlottieJsonObject,
  RlottieSource,
} from './types';

export type NormalizeResult =
  | {ok: true; source: NormalizedRlottieSource}
  | {ok: false; code: RlottieErrorCode; message: string};

/**
 * URI schemes the native resolvers accept unconditionally. `https:` is handled
 * separately (see `normalizeUri`): it is gated behind `allowRemoteSources`
 * rather than always rejected or always allowed. `http:` (no TLS) is never
 * accepted, flag or no flag — plan §16 mandates HTTPS-only for remote content.
 *
 * These three get a dedicated message rather than the generic "unsupported
 * scheme", because "why doesn't a URL work" is the obvious first question.
 */
const ALLOWED_SCHEMES = ['file:', 'asset:', 'content:'] as const;

const SCHEME_RE = /^([a-zA-Z][a-zA-Z0-9+.-]*):/;

/**
 * Identity-memoized normalization for object sources. A WeakMap means a source
 * object that falls out of scope takes its cached string and digest with it.
 *
 * Deliberately NOT applied to string sources: keying a Map by a multi-megabyte
 * JSON string would retain that string forever, and a caller holding a stable
 * string can already hoist it.
 */
const objectSourceCache = new WeakMap<RlottieJsonObject, NormalizeResult>();

function fail(code: RlottieErrorCode, message: string): NormalizeResult {
  return {ok: false, code, message};
}

/**
 * Derives the TS half of the cache key.
 *
 * Content-addressed for JSON sources: `sha256(json)` guarantees two different
 * payloads cannot collide regardless of what the caller passed as `cacheKey`.
 * The caller's key is appended (not substituted) so the same bytes under
 * different logical identities stay distinguishable.
 *
 * END-TO-END SCHEME (Chunk 5.3, plan §15) — this function produces only
 * `sha256(jsonBytes):callerCacheKey`. That string is what crosses the bridge
 * as `NormalizedRlottieSource.cacheKey`; native completes the formula by
 * appending `:rlottieCommit:parseConfig` before the result reaches rlottie's
 * own model cache. The split is a synchronicity constraint, not a temporary
 * gap: `rlottieCommit` (`cpp/RlottieVersion.h`) and `parseConfig` are only
 * knowable natively, and `normalizeSource` must stay synchronous (it runs
 * during React render — see `RlottieView.tsx`), so it can never await a
 * native round-trip to fold them in itself. Concretely:
 *   - TS contributes: `sha256(jsonBytes) : callerCacheKey`  (this function)
 *   - native appends: `: rlottieCommit : parseConfig`        (Chunk 5.3, native half)
 * Do not change this `sha256(json):callerKey` format without updating the
 * native composition in lockstep — `tests/js/source.test.js` pins it, and the
 * native side is written against exactly this shape.
 */
function cacheKeyForJson(json: string, callerKey: string | undefined): string {
  return `${sha256Hex(json)}:${callerKey ?? ''}`;
}

/**
 * For file-backed sources (`uri`/`path`) the content is not available in JS —
 * reading the file synchronously on the JS thread to hash it would be its own
 * performance bug, and doing it asynchronously would make `normalizeSource`
 * async, which `RlottieView.tsx` requires it not be (see `cacheKeyForJson`).
 * So the TS-knowable half of the key is the location plus the caller's label:
 * `sha256(location):callerCacheKey`, mirroring the JSON scheme's shape so both
 * kinds compose with native's `:rlottieCommit:parseConfig` suffix identically.
 *
 * KNOWN WEAKNESS, left for native (Chunk 5.3, native half): this key is
 * location-derived, not content-derived, so two different files written
 * successively to the same path produce the SAME key — unlike the JSON path,
 * TS has no cheap way to detect that a `file://`/`content://` target changed
 * underneath a stable path. Native is the only layer positioned to close this,
 * because it already reads the file's bytes to parse it and can incorporate
 * what it observes then (e.g. size + mtime, or hashing the bytes already in
 * hand) into the part of the key it appends — no new native API is implied
 * beyond what the resolver already does per read. Until native does that, a
 * cache entry for a file-backed source does not invalidate when the file at
 * that path changes without also changing `cacheKey` or the path itself.
 */
function cacheKeyForLocation(
  location: string,
  callerKey: string | undefined,
): string {
  return `${sha256Hex(location)}:${callerKey ?? ''}`;
}

function normalizeJson(
  json: string | RlottieJsonObject,
  callerKey: string | undefined,
  resourcePath: string | undefined,
): NormalizeResult {
  let text: string;
  if (typeof json === 'string') {
    text = json;
  } else {
    try {
      // THE single stringify (plan §12). Everything downstream — the digest, the
      // bridge payload, the native parse — consumes this exact string.
      text = JSON.stringify(json);
    } catch {
      // Circular references, or a toJSON() that throws.
      return fail(
        'INVALID_SOURCE',
        'source.json could not be serialized to JSON',
      );
    }
  }

  if (text.length === 0) {
    return fail('INVALID_SOURCE', 'source.json is empty');
  }

  const normalized: NormalizedRlottieSource = {
    json: text,
    cacheKey: cacheKeyForJson(text, callerKey),
  };
  if (resourcePath !== undefined) {
    // Validated natively (canonicalized + confined to app-private storage);
    // passing it through unchecked here would duplicate that logic in a place
    // that cannot see the filesystem.
    return {ok: true, source: {...normalized, resourcePath}};
  }
  return {ok: true, source: normalized};
}

/**
 * Chunk 5.4 — the remote-loading gate (stub).
 *
 * This function decides only whether a `source.uri` scheme is admissible; it
 * never performs network I/O and never will from here (plan §12/§16 — the C++
 * core is the layer that would eventually fetch, and even that is v1.1 scope).
 * `allowRemoteSources` therefore changes REJECTION WORDING, not behavior: an
 * `https://` uri is `INVALID_SOURCE` whether the flag is set or not, but the
 * message differs so a developer who has already opted in isn't told their
 * URL looks malformed when the real reason is "not implemented yet".
 * `http://` is rejected unconditionally — plan §16 mandates HTTPS-only for any
 * future remote loading, and that is not something a consumer can opt out of.
 */
function normalizeUri(
  uri: string,
  callerKey: string | undefined,
  allowRemoteSources: boolean,
): NormalizeResult {
  const match = SCHEME_RE.exec(uri);
  if (!match) {
    return fail(
      'INVALID_SOURCE',
      'source.uri has no scheme — use file://, asset:///, or content:// (Android)',
    );
  }
  const scheme = `${match[1].toLowerCase()}:`;

  if (scheme === 'http:') {
    return fail(
      'INVALID_SOURCE',
      'source.uri uses http:// — remote sources must use https:// (plain HTTP is never allowed, even with allowRemoteSources), and remote loading is not implemented yet in this version — download the animation and pass a file:// uri',
    );
  }
  if (scheme === 'https:') {
    if (!allowRemoteSources) {
      return fail(
        'INVALID_SOURCE',
        'source.uri is a remote https:// url, which requires the allowRemoteSources prop — but remote loading is not implemented yet in this version even with it set; download the animation and pass a file:// uri',
      );
    }
    return fail(
      'INVALID_SOURCE',
      'allowRemoteSources is set, but remote loading is not implemented yet in this version — the native layer never performs network I/O in v1 (v1.1 scope); download the animation and pass a file:// uri',
    );
  }
  if (!(ALLOWED_SCHEMES as readonly string[]).includes(scheme)) {
    return fail('INVALID_SOURCE', `unsupported source.uri scheme "${scheme}"`);
  }

  return {
    ok: true,
    source: {uri, cacheKey: cacheKeyForLocation(uri, callerKey)},
  };
}

/**
 * Normalizes a `source` prop into what crosses the bridge.
 *
 * Pure and synchronous. Object sources are memoized on identity, so calling
 * this on every render is cheap for a stable source and expensive only for a
 * caller who allocates a new object literal each time.
 *
 * `allowRemoteSources` (Chunk 5.4, default `false`) only affects the message
 * on an `https://` `source.uri` — see `normalizeUri`. It does not change
 * anything for `json`/`path` sources, and is intentionally NOT part of the
 * memoization key for object `json` sources: those never reach the scheme
 * check at all, so the flag cannot affect their result.
 */
export function normalizeSource(
  source: RlottieSource,
  allowRemoteSources: boolean = false,
): NormalizeResult {
  if (source === null || source === undefined) {
    return fail('INVALID_SOURCE', 'source is required');
  }

  // A require()d asset id. Note that `require('./anim.json')` does NOT land
  // here: Metro inlines JSON as an object, which takes the `json` path below.
  // This branch is for assets registered with the asset registry.
  if (typeof source === 'number') {
    const resolved = Image.resolveAssetSource(source);
    if (
      !resolved ||
      typeof resolved.uri !== 'string' ||
      resolved.uri.length === 0
    ) {
      return fail(
        'INVALID_SOURCE',
        'could not resolve the required asset to a uri',
      );
    }
    return normalizeUri(resolved.uri, undefined, allowRemoteSources);
  }

  if (typeof source !== 'object') {
    return fail(
      'INVALID_SOURCE',
      `source must be an object or a require()d asset id`,
    );
  }

  if ('json' in source && source.json !== undefined) {
    const {json, cacheKey, resourcePath} = source;

    if (typeof json === 'object' && json !== null) {
      const cached = objectSourceCache.get(json);
      if (cached) {
        return cached;
      }
      const result = normalizeJson(json, cacheKey, resourcePath);
      objectSourceCache.set(json, result);
      return result;
    }
    if (typeof json !== 'string') {
      return fail(
        'INVALID_SOURCE',
        'source.json must be a string or an object',
      );
    }
    return normalizeJson(json, cacheKey, resourcePath);
  }

  if ('uri' in source && source.uri !== undefined) {
    if (typeof source.uri !== 'string' || source.uri.length === 0) {
      return fail('INVALID_SOURCE', 'source.uri must be a non-empty string');
    }
    return normalizeUri(source.uri, source.cacheKey, allowRemoteSources);
  }

  if ('path' in source && source.path !== undefined) {
    if (typeof source.path !== 'string' || source.path.length === 0) {
      return fail('INVALID_SOURCE', 'source.path must be a non-empty string');
    }
    // Passed through as-is; the native resolvers canonicalize it and reject
    // anything outside app-private storage.
    return {
      ok: true,
      source: {
        path: source.path,
        cacheKey: cacheKeyForLocation(source.path, source.cacheKey),
      },
    };
  }

  return fail(
    'INVALID_SOURCE',
    'source must have one of `json`, `uri`, or `path`',
  );
}

/**
 * True when two sources would produce the same native load — used by the view
 * to avoid re-issuing a load on an unrelated re-render.
 */
export function isSameNormalizedSource(
  a: NormalizedRlottieSource | undefined,
  b: NormalizedRlottieSource | undefined,
): boolean {
  if (a === undefined || b === undefined) {
    return a === b;
  }
  if (a.cacheKey !== b.cacheKey) {
    return false;
  }
  const aKind = 'json' in a ? 'json' : 'uri' in a ? 'uri' : 'path';
  const bKind = 'json' in b ? 'json' : 'uri' in b ? 'uri' : 'path';
  if (aKind !== bKind) {
    return false;
  }
  // Same kind and same content-derived key; resourcePath is the only remaining
  // input that changes what native loads.
  const aResource = 'json' in a ? a.resourcePath : undefined;
  const bResource = 'json' in b ? b.resourcePath : undefined;
  return aResource === bResource;
}
