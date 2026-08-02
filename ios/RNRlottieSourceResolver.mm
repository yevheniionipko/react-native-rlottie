// Chunk 2.4/5.2 — see RNRlottieSourceResolver.h for the contract this implements.
#import "RNRlottieSourceResolver.h"

#include <sys/stat.h>

#include "ErrorCode.h"
#include "InputLimits.h"
#include "PlayerError.h"

NS_ASSUME_NONNULL_BEGIN

NSString *const RNRlottieSourceResolverErrorKey = @"__rnrlottie_resolver_error";

namespace {

// The live, process-global InputLimits policy (Chunk 5.1's
// `rnrlottie::currentInputLimits()`, cpp/InputLimits.h) — reading this instead
// of constructing a fresh `InputLimits{}` locally means an app-level
// `RlottieModule.configure` override (should one ever set input limits, not
// just the model cache size) is honored here too, and there is exactly one
// source of truth shared with the C++ core and Android's RlottieJni.cpp.
rnrlottie::InputLimits CurrentLimits() { return rnrlottie::currentInputLimits(); }

NSString *ErrorCodeString(rnrlottie::PlayerErrorCode code) {
  return [NSString stringWithUTF8String:rnrlottie::errorCodeString(code)];
}

// Lexically standardizes `rawPath` (expands "~", collapses "." / ".."
// components, removes redundant slashes) and, if the path currently exists,
// additionally resolves symlinks — catching the case where a symlink (not a
// literal ".." segment) would otherwise walk the result outside the sandbox.
// A nonexistent path still gets the lexical pass, which is enough to reject
// a `..`-traversal attempt without requiring the file to exist first (we
// want SOURCE_NOT_FOUND, not a false "escaped the sandbox", for a merely
// missing file under an otherwise-legitimate path).
NSString *_Nullable Canonicalize(NSString *rawPath) {
  if (rawPath.length == 0) {
    return nil;
  }
  NSString *standardized = rawPath.stringByStandardizingPath;
  if ([[NSFileManager defaultManager] fileExistsAtPath:standardized]) {
    standardized = standardized.stringByResolvingSymlinksInPath;
  }
  return standardized;
}

// Canonicalizes `root` itself (roots like NSHomeDirectory() are frequently a
// symlink on iOS — e.g. /var -> /private/var) so prefix comparison against an
// already-canonicalized candidate path is meaningful.
NSString *CanonicalRoot(NSString *root) {
  NSString *resolved = root.stringByResolvingSymlinksInPath;
  if (![resolved hasSuffix:@"/"]) {
    resolved = [resolved stringByAppendingString:@"/"];
  }
  return resolved;
}

BOOL PathIsWithinRoot(NSString *canonicalPath, NSString *canonicalRootWithSlash) {
  if (canonicalPath.length == 0 || canonicalRootWithSlash.length == 0) {
    return NO;
  }
  NSString *withoutTrailingSlash = [canonicalRootWithSlash substringToIndex:canonicalRootWithSlash.length - 1];
  return [canonicalPath isEqualToString:withoutTrailingSlash] || [canonicalPath hasPrefix:canonicalRootWithSlash];
}

// The only two roots a resolved path may live under: the app's sandbox
// container (Documents/Library/tmp/...) and the main bundle (read-only app
// resources). Resolved once per call — cheap (a couple of syscalls), and
// avoids caching a value that could go stale across app-container moves.
NSArray<NSString *> *SandboxRoots(void) {
  return @[
    CanonicalRoot(NSHomeDirectory()),
    CanonicalRoot([[NSBundle mainBundle] bundlePath]),
  ];
}

BOOL PathIsSandboxed(NSString *canonicalPath) {
  for (NSString *root in SandboxRoots()) {
    if (PathIsWithinRoot(canonicalPath, root)) {
      return YES;
    }
  }
  return NO;
}

// Canonicalizes + sandbox-checks `rawPath`, then stats it against
// InputLimits::maxJsonBytes. On success returns the canonical path; on
// failure returns nil with `outErrorCode`/`outErrorMessage` populated.
NSString *_Nullable ValidateFilePath(NSString *rawPath,
                                      NSString *_Nullable *_Nonnull outErrorCode,
                                      NSString *_Nullable *_Nonnull outErrorMessage) {
  NSString *canonical = Canonicalize(rawPath);
  if (!canonical || [canonical rangeOfString:@".."].location != NSNotFound) {
    // Either failed to standardize, or a literal ".." survived
    // standardization (stringByStandardizingPath does not resolve leading
    // ".." components that would walk above a root that doesn't exist as a
    // real filesystem prefix yet) — reject rather than guess.
    *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::InvalidSource);
    *outErrorMessage = @"source path failed to canonicalize";
    return nil;
  }
  if (!PathIsSandboxed(canonical)) {
    *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::InvalidSource);
    *outErrorMessage = @"source path resolves outside the app sandbox/bundle";
    return nil;
  }

  struct stat st;
  if (stat(canonical.fileSystemRepresentation, &st) != 0) {
    *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::SourceNotFound);
    *outErrorMessage = @"source file does not exist";
    return nil;
  }
  if (!S_ISREG(st.st_mode)) {
    *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::SourceNotFound);
    *outErrorMessage = @"source path is not a regular file";
    return nil;
  }

  if (static_cast<unsigned long long>(st.st_size) >
      static_cast<unsigned long long>(CurrentLimits().maxJsonBytes)) {
    *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::SourceTooLarge);
    *outErrorMessage = @"source file exceeds the configured byte limit";
    return nil;
  }

  return canonical;
}

// Raster-asset extensions rlottie's external-image path can actually consume.
// v1 has no image-loader module built in (LOTTIE_MODULE OFF, plan §14), but
// this allow-list is defense-in-depth for whatever raster decoder a resource
// directory's files are eventually handed to, and matches plan §12's "allow
// only known image extensions". png/jpg/jpeg cover the formats Lottie/After
// Effects exporters actually emit for embedded raster assets; webp is
// included because it's the other format popular Lottie tooling (e.g.
// lottie-web asset pipelines) commonly emits. Nothing else is a legitimate
// rlottie external asset, so anything else fails the whole resourcePath
// closed rather than being silently skipped.
BOOL HasAllowedImageExtension(NSString *filename) {
  static NSSet<NSString *> *const kAllowed =
      [NSSet setWithArray:@[ @"png", @"jpg", @"jpeg", @"webp" ]];
  return [kAllowed containsObject:filename.pathExtension.lowercaseString];
}

// True when `canonicalPath` (already trailing-slash-normalized the same way
// `SandboxRoots()` entries are) IS one of the shared sandbox roots itself,
// rather than a directory nested under one. `resourcePath` must be a
// dedicated per-animation-package directory (plan §12 "create one private
// directory per animation package") — handing back the bare root would let
// two unrelated animations share (and read each other's) asset storage.
BOOL PathIsExactlyASandboxRoot(NSString *canonicalPath) {
  NSString *withSlash = [canonicalPath hasSuffix:@"/"] ? canonicalPath
                                                        : [canonicalPath stringByAppendingString:@"/"];
  for (NSString *root in SandboxRoots()) {
    if ([withSlash isEqualToString:root]) {
      return YES;
    }
  }
  return NO;
}

// Validates `rawPath` as rlottie's external-asset `resourcePath` root (plan
// §12 / §16, Chunk 5.2):
//   - canonicalized (symlinks resolved when the path exists), sandboxed, and
//     NOT a bare shared root (must be a dedicated per-package directory);
//   - every entry under it, recursively: no symlinks (a conservative
//     superset of "reject symlinks that escape the directory" — this
//     directory should contain nothing but the raster assets the caller
//     placed there, so no symlink is ever legitimate here); regular files
//     only, with an allow-listed image extension;
//   - bounded file count and total bytes (InputLimits::maxExternalAssets /
//     maxExternalBytes).
// Returns the canonical directory path on success, nil + populated
// out-params on any rejection.
NSString *_Nullable ValidateResourceDirectory(NSString *rawPath,
                                               NSString *_Nullable *_Nonnull outErrorCode,
                                               NSString *_Nullable *_Nonnull outErrorMessage) {
  NSString *canonical = Canonicalize(rawPath);
  if (!canonical || [canonical rangeOfString:@".."].location != NSNotFound) {
    *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::InvalidSource);
    *outErrorMessage = @"resourcePath failed to canonicalize";
    return nil;
  }
  if (!PathIsSandboxed(canonical)) {
    *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::InvalidSource);
    *outErrorMessage = @"resourcePath resolves outside the app sandbox/bundle";
    return nil;
  }
  if (PathIsExactlyASandboxRoot(canonical)) {
    *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::InvalidSource);
    *outErrorMessage = @"resourcePath must be a dedicated per-animation directory, not a shared root";
    return nil;
  }

  NSFileManager *fm = [NSFileManager defaultManager];
  BOOL isDirectory = NO;
  BOOL exists = [fm fileExistsAtPath:canonical isDirectory:&isDirectory];
  if (!exists) {
    *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::SourceNotFound);
    *outErrorMessage = @"resourcePath directory does not exist";
    return nil;
  }
  if (!isDirectory) {
    *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::InvalidSource);
    *outErrorMessage = @"resourcePath is not a directory";
    return nil;
  }

  NSURL *rootURL = [NSURL fileURLWithPath:canonical isDirectory:YES];
  NSArray<NSURLResourceKey> *keys = @[
    NSURLIsSymbolicLinkKey, NSURLIsRegularFileKey, NSURLIsDirectoryKey, NSURLFileSizeKey
  ];
  NSDirectoryEnumerator<NSURL *> *enumerator =
      [fm enumeratorAtURL:rootURL
          includingPropertiesForKeys:keys
                             options:NSDirectoryEnumerationSkipsHiddenFiles
                        errorHandler:nil];

  const rnrlottie::InputLimits &limits = CurrentLimits();
  std::size_t fileCount = 0;
  unsigned long long totalBytes = 0;

  for (NSURL *itemURL in enumerator) {
    NSNumber *isSymlink = nil;
    [itemURL getResourceValue:&isSymlink forKey:NSURLIsSymbolicLinkKey error:nil];
    if (isSymlink.boolValue) {
      *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::InvalidSource);
      *outErrorMessage = @"resourcePath must not contain symlinks";
      return nil;
    }

    NSNumber *isDir = nil;
    [itemURL getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
    if (isDir.boolValue) {
      continue;  // subdirectories are allowed to nest assets; recursed into below.
    }

    NSNumber *isRegular = nil;
    [itemURL getResourceValue:&isRegular forKey:NSURLIsRegularFileKey error:nil];
    if (!isRegular.boolValue) {
      *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::InvalidSource);
      *outErrorMessage = @"resourcePath must contain only regular files";
      return nil;
    }

    if (!HasAllowedImageExtension(itemURL.lastPathComponent)) {
      *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::InvalidSource);
      *outErrorMessage = @"resourcePath may only contain png/jpg/jpeg/webp assets";
      return nil;
    }

    fileCount += 1;
    if (fileCount > limits.maxExternalAssets) {
      *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::SourceTooLarge);
      *outErrorMessage = @"resourcePath exceeds the configured asset-count limit";
      return nil;
    }

    NSNumber *fileSize = nil;
    [itemURL getResourceValue:&fileSize forKey:NSURLFileSizeKey error:nil];
    totalBytes += fileSize.unsignedLongLongValue;
    if (totalBytes > static_cast<unsigned long long>(limits.maxExternalBytes)) {
      *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::SourceTooLarge);
      *outErrorMessage = @"resourcePath exceeds the configured total-bytes limit";
      return nil;
    }
  }

  return canonical;
}

// Resolves an `asset:///<relative/path>` URI against the main bundle.
//
// iOS has no standard "app bundle resource" URI scheme, so one had to be
// chosen. It is `asset://` — the SAME scheme Android's RlottieSourceResolver
// uses for its `assets/` root (and RN's own convention for bundled non-image
// assets) — so a single JS `source` value works unchanged on both platforms.
// The two roots differ (main bundle here, AssetManager there); the scheme
// deliberately does not.
//
// The URI's path component, minus its leading slash, is treated as relative to
// `[[NSBundle mainBundle] bundlePath]`, then goes through the same
// canonicalize + sandbox + stat pipeline as a `file://` URI, so a `..` in the
// relative path cannot escape the bundle.
NSString *_Nullable ResolveAssetURI(NSURL *url,
                                     NSString *_Nullable *_Nonnull outErrorCode,
                                     NSString *_Nullable *_Nonnull outErrorMessage) {
  NSString *relative = url.path;  // e.g. "/animations/foo.json"
  if ([relative hasPrefix:@"/"]) {
    relative = [relative substringFromIndex:1];
  }
  if (relative.length == 0) {
    *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::InvalidSource);
    *outErrorMessage = @"asset:// URI has no resource path";
    return nil;
  }
  NSString *candidate = [[[NSBundle mainBundle] bundlePath] stringByAppendingPathComponent:relative];
  return ValidateFilePath(candidate, outErrorCode, outErrorMessage);
}

}  // namespace

@implementation RNRlottieSourceResolver

+ (nullable NSDictionary<NSString *, id> *)resolveSource:(NSDictionary<NSString *, id> *)source
                                                errorCode:(NSString *_Nullable *_Nonnull)outErrorCode
                                             errorMessage:(NSString *_Nullable *_Nonnull)outErrorMessage {
  *outErrorCode = nil;
  *outErrorMessage = nil;

  if (![source isKindOfClass:[NSDictionary class]]) {
    *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::InvalidSource);
    *outErrorMessage = @"`source` must be an object";
    return nil;
  }

  // --- {json: "...", cacheKey?, resourcePath?} -------------------------
  id jsonValue = source[@"json"];
  if ([jsonValue isKindOfClass:[NSString class]] && [(NSString *)jsonValue length] > 0) {
    NSString *json = (NSString *)jsonValue;
    // Never log `json` itself anywhere in this file (plan §16).
    const std::size_t jsonBytes = [json lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    rnrlottie::PlayerError sizeError = rnrlottie::checkJsonSize(jsonBytes, CurrentLimits());
    if (sizeError) {
      *outErrorCode = ErrorCodeString(sizeError.code);
      *outErrorMessage = @"animation JSON exceeds the configured byte limit";
      return nil;
    }

    NSMutableDictionary<NSString *, id> *resolved = [NSMutableDictionary dictionary];
    resolved[@"json"] = json;
    id cacheKeyValue = source[@"cacheKey"];
    resolved[@"cacheKey"] = [cacheKeyValue isKindOfClass:[NSString class]] ? cacheKeyValue : @"";

    id resourcePathValue = source[@"resourcePath"];
    if ([resourcePathValue isKindOfClass:[NSString class]] && [(NSString *)resourcePathValue length] > 0) {
      // "must be a validated app-private directory or empty — never a
      // caller-supplied arbitrary root" (bridge-contract.md), hardened per
      // plan §12/§16 (Chunk 5.2): a dedicated per-package directory, no
      // traversal/symlink escape, image-extension allow-list, file-count and
      // total-byte caps. Fail closed: any rejection here rejects the whole
      // source rather than silently dropping resourcePath.
      NSString *canonicalDir =
          ValidateResourceDirectory((NSString *)resourcePathValue, outErrorCode, outErrorMessage);
      if (!canonicalDir) {
        return nil;
      }
      resolved[@"resourcePath"] = canonicalDir;
    }
    return resolved;
  }

  // --- {path: "..."} (defensive passthrough) ----------------------------
  id pathValue = source[@"path"];
  if ([pathValue isKindOfClass:[NSString class]] && [(NSString *)pathValue length] > 0) {
    NSString *canonical = ValidateFilePath((NSString *)pathValue, outErrorCode, outErrorMessage);
    if (!canonical) {
      return nil;
    }
    return @{@"path" : canonical};
  }

  // --- {uri: "..."} -------------------------------------------------------
  id uriValue = source[@"uri"];
  if ([uriValue isKindOfClass:[NSString class]] && [(NSString *)uriValue length] > 0) {
    NSString *uriString = (NSString *)uriValue;
    NSURL *url = [NSURL URLWithString:uriString];
    NSString *scheme = url.scheme.lowercaseString;

    if ([scheme isEqualToString:@"file"]) {
      NSString *canonical = ValidateFilePath(url.path ?: @"", outErrorCode, outErrorMessage);
      if (!canonical) {
        return nil;
      }
      return @{@"path" : canonical};
    }

    if ([scheme isEqualToString:@"asset"]) {
      NSString *canonical = ResolveAssetURI(url, outErrorCode, outErrorMessage);
      if (!canonical) {
        return nil;
      }
      return @{@"path" : canonical};
    }

    if ([scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"]) {
      *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::InvalidSource);
      *outErrorMessage = @"remote (http/https) sources are not supported in v1";
      return nil;
    }

    if ([scheme isEqualToString:@"content"]) {
      *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::InvalidSource);
      *outErrorMessage = @"content:// sources are Android-only";
      return nil;
    }

    *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::InvalidSource);
    *outErrorMessage = @"unsupported or unrecognized `uri` scheme";
    return nil;
  }

  *outErrorCode = ErrorCodeString(rnrlottie::PlayerErrorCode::InvalidSource);
  *outErrorMessage = @"`source` must be one of {json}, {path}, or {uri}";
  return nil;
}

@end

NS_ASSUME_NONNULL_END
