// Chunk 2.4 — RNRlottieSourceResolver: turns the raw JS `source` prop into
// the two shapes the native player understands (docs/bridge-contract.md
// "Source resolution boundary" / "Resolver contract (Chunks 2.4 / 3.4)"):
//
//   {json: NSString, cacheKey: NSString, resourcePath: NSString?}  -> setSourceData
//   {path: NSString}                                               -> setSourceFile
//
// or a typed failure (a `PlayerErrorCode`-derived code string from
// cpp/ErrorCode.h + a developer message). This is the iOS counterpart to
// Android's `RlottieSourceResolver.kt` — the plan's iOS file list omits it,
// but the bridge contract calls that an oversight, not a design choice: both
// platforms must accept/reject the same source kinds so JS behaviour is
// identical (see the contract's "Resolver contract" section for the exact
// table this class implements).
//
// v1 accepted input shapes (from the JS-facing `RlottieSource` union, plan
// §3 — `{json, cacheKey}` / `{uri, cacheKey}`; a bare `{path}` is also
// accepted defensively in case an upstream layer ever resolves ahead of
// time):
//   - {json: "<raw json string>", cacheKey?}   -> validated + passed through.
//   - {uri: "file:///…"}                        -> canonicalized, sandboxed,
//                                                  stat'd, -> {path}.
//   - {uri: "asset:///<relative/path>"}         -> resolved against the main
//                                                  bundle, -> {path}. Same
//                                                  scheme as Android's
//                                                  `assets/` root, so one JS
//                                                  source value works on both
//                                                  platforms (the roots
//                                                  differ; the scheme does
//                                                  not). See
//                                                  docs/bridge-contract.md.
//   - {path: "/already/resolved/path.json"}    -> canonicalized + sandboxed
//                                                  the same as a file:// uri.
//
// v1 REJECTS (INVALID_SOURCE), never falling back to a different kind:
//   - http(s):// (remote fetching is v1.1, plan §3)
//   - content:// (Android-only)
//   - any other/unknown scheme
//   - anything with `..` traversal or that canonicalizes outside the app
//     sandbox (NSHomeDirectory()) or the main bundle
//
// Security (plan §16 / CLAUDE.md): never performs network I/O; never logs
// raw animation JSON; every path is canonicalized (symlinks resolved) and
// checked for sandbox/bundle containment *before* any stat/read; the
// InputLimits::maxJsonBytes byte limit (cpp/InputLimits.h) is enforced via
// `stat` *before* a full file is ever read into memory.
//
// Threading: stateless, safe to call from any thread. Called synchronously
// from RNRlottieViewManager's `source` custom prop setter (main thread, per
// that file's threading contract) — every I/O op here is either a cheap
// `stat`/symlink-resolve or a single bounded read capped by
// InputLimits::maxJsonBytes, so it stays cheap enough to not warrant hopping
// off main for v1.
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Reserved key RNRlottieViewManager uses to smuggle a resolver failure through
// `RNRlottieView.pendingSource` without changing that class's public
// interface (Chunk 2.3's header is left untouched by this chunk).
// `-[RNRlottieView flushPendingSourceIfNeeded]` checks for this key first and,
// if present, emits `onAnimationError` with the embedded {code, message}
// verbatim instead of attempting to apply the dictionary as a source.
extern NSString *const RNRlottieSourceResolverErrorKey;

@interface RNRlottieSourceResolver : NSObject

// Resolves `source` (the raw dictionary RCTConvert produced from the JS
// `source` prop) into a native-ready dictionary. Returns nil and populates
// `outErrorCode` (a cpp/ErrorCode.h string) / `outErrorMessage` on any
// rejection; a nil `source` is not an error (callers should treat it as "no
// source", matching Chunk 2.3's existing "JS cleared `source`" no-op path —
// this method is simply not called in that case).
+ (nullable NSDictionary<NSString *, id> *)resolveSource:(NSDictionary<NSString *, id> *)source
                                                errorCode:(NSString *_Nullable *_Nonnull)outErrorCode
                                             errorMessage:(NSString *_Nullable *_Nonnull)outErrorMessage;

@end

NS_ASSUME_NONNULL_END
