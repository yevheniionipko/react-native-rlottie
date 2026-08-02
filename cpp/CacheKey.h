// Chunk 5.3 — deterministic cache keys, native half.
//
// Plan §15's internal key is:
//   sha256(jsonBytes) + ":" + callerCacheKey + ":" + rlottieCommit + ":" + parseConfig
//
// The TS layer (src/source.ts, cacheKeyForJson) computes `sha256(json):callerKey`
// and deliberately stops there: it can't see the vendored rlottie commit
// (cpp/RlottieVersion.h) synchronously, and appending it would make source
// normalization async. This composes the rest, natively, in
// RlottiePlayerCore::loadFromData.
//
// CRITICAL PROPERTY: composeCacheKey recomputes sha256 over the ACTUAL bytes
// about to be parsed rather than trusting the `callerCacheKey` string handed
// in (which already contains a TS-computed hash, but this function treats it
// as opaque). Because that native hash is the first ':'-delimited field, and
// hex-sha256 output can never itself contain a ':', two calls with different
// `json` always produce keys that diverge at the very first field — no matter
// what a caller passes as `callerCacheKey`. A caller cannot make two different
// payloads alias the same key, whether by accident (a stale/wrong TS hash) or
// by supplying the same label for both.
//
// SCOPE — File-backed sources: rlottie::Animation::loadFromFile() (see
// cpp/third_party/rlottie/inc/rlottie.h) takes no key parameter at all; rlottie
// keys its own internal model cache for that path by the (already-
// canonicalized, app-private) path string. composeCacheKey is therefore only
// used on the Data (raw-JSON) path — see the Kind::File branch of
// RenderCoordinator::runLoad. This carries forward the known limitation
// documented in src/source.ts's cacheKeyForLocation: a file overwritten in
// place between two loads at the same path is not distinguished by rlottie's
// cache. Fixing that would mean the core reading+hashing the file itself
// before rlottie ever sees it — a change to how File-kind sources are loaded,
// not just to key composition — and is out of scope for this chunk.
//
// Header-only (inline): see the note in InputLimits.h — both consuming
// CMakeLists enumerate cpp/*.cpp explicitly and a new .cpp is out of scope.
#pragma once

#include <string>

#include "Sha256.h"

namespace rnrlottie {

// The "parseConfig" component: anything besides the JSON bytes that can
// change what the parsed model looks like for byte-identical input.
// Version-prefixed so a future field addition changes this component (and
// therefore every key) instead of silently colliding with old cache entries.
// Currently just `resourcePath`, since it changes how embedded external
// raster-asset references resolve into the parsed model.
inline std::string composeParseConfig(const std::string& resourcePath) {
    return "v1:" + resourcePath;
}

// Pure (no dependency on RlottieVersion.h) so it is independently testable
// against an arbitrary `rlottieCommit` string — proving "the key changes when
// the rlottie commit changes" does not require actually rebuilding against a
// different vendored revision. Production callers pass kRlottieCommit
// (cpp/RlottieVersion.h); see RlottiePlayerCore::loadFromData.
inline std::string composeCacheKey(const std::string& json, const std::string& callerCacheKey,
                                   const std::string& rlottieCommit,
                                   const std::string& resourcePath) {
    return sha256Hex(json) + ":" + callerCacheKey + ":" + rlottieCommit + ":" +
           composeParseConfig(resourcePath);
}

}  // namespace rnrlottie
