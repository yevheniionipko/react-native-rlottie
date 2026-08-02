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
 * URI schemes the native resolvers accept. `http`/`https` are deliberately NOT
 * here: remote loading is v1.1 (plan §3), gated behind `allowRemoteSources` in
 * Chunk 5.4, and the C++ core never performs network I/O (plan §16). They get a
 * dedicated message rather than the generic "unsupported scheme", because
 * "why doesn't a URL work" is the obvious first question.
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
 * Derives the cache key.
 *
 * Content-addressed for JSON sources: `sha256(json)` guarantees two different
 * payloads cannot collide regardless of what the caller passed as `cacheKey`.
 * The caller's key is appended (not substituted) so the same bytes under
 * different logical identities stay distinguishable.
 *
 * SCOPE — the full formula from plan §15 is
 *   `sha256(jsonBytes) : callerCacheKey : rlottieCommit : parseConfig`
 * and Chunk 5.3 owns completing it. The last two components are deliberately
 * NOT added here: `rlottieCommit` lives in `cpp/RlottieVersion.h` and only
 * reaches JS through the async `getNativeVersion()`, so including it would make
 * normalization async. They belong on the native side, which knows both
 * synchronously. Until then a cache entry does not survive an rlottie bump —
 * see the Chunk 5.3 note in CLAUDE.md.
 */
function cacheKeyForJson(json: string, callerKey: string | undefined): string {
  return `${sha256Hex(json)}:${callerKey ?? ''}`;
}

/**
 * For file-backed sources the content is not available in JS, so the key is the
 * location plus the caller's label. Chunk 5.3 should strengthen this natively
 * (size + mtime, or hashing the bytes it already reads) — two different files
 * successively at the same path currently produce the same key.
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

function normalizeUri(
  uri: string,
  callerKey: string | undefined,
): NormalizeResult {
  const match = SCHEME_RE.exec(uri);
  if (!match) {
    return fail(
      'INVALID_SOURCE',
      'source.uri has no scheme — use file://, asset:///, or content:// (Android)',
    );
  }
  const scheme = `${match[1].toLowerCase()}:`;

  if (scheme === 'http:' || scheme === 'https:') {
    return fail(
      'INVALID_SOURCE',
      'remote sources are not supported in v1 — download the animation and pass a file:// uri',
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
 */
export function normalizeSource(source: RlottieSource): NormalizeResult {
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
    return normalizeUri(resolved.uri, undefined);
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
    return normalizeUri(source.uri, source.cacheKey);
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
