package com.rlottie

import android.content.Context
import android.net.Uri
import com.facebook.react.bridge.ReadableMap
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.nio.file.Files

/**
 * Chunk 3.4/5.1/5.2 — turns a JS `source` prop into an app-controlled path or
 * JSON string BEFORE the C++ core sees it (docs/bridge-contract.md, "Resolver
 * contract"; plan §12/§16). The core never sees a URI and never does I/O over
 * the network; every accepted/rejected kind here must match
 * `ios/RNRlottieSourceResolver` byte-for-byte (same accepted kinds, same
 * rejected kinds, same error codes).
 *
 * Accepted (v1):
 *  - `{json, cacheKey?, resourcePath?}` -> [RlottieResolvedSource.Data]
 *  - `{path}` -> a pre-resolved absolute path, still confined + validated here
 *  - `{uri: "file:///…"}` -> canonicalized, confined to an app-private dir
 *  - `{uri: "content://…"}` -> copied into app-private storage
 *  - `{uri: "asset:///…"}` -> a bundled `assets/` resource, copied into
 *    app-private storage (there is no `require()`-style numeric asset id on
 *    the native side; that normalization is TypeScript's job in Chunk 4.1)
 *
 * Rejected (v1, `INVALID_SOURCE` — v1.1 territory per plan §3):
 *  - `http://` / `https://` remote URIs (no HTTP from native; opt-in remote
 *    loading is Chunk 5.4)
 *  - `.lottie` containers (zip; not a bundled rlottie feature yet)
 *  - any URI scheme not on the allow-list above
 *
 * Security rules enforced here (plan §16, contract doc):
 *  - every path is canonicalized (`File.canonicalFile`) and must resolve
 *    inside one of this app's private directories; a `..` segment, a symlink
 *    escape, or any other out-of-sandbox resolution is a rejection, not a
 *    clamp;
 *  - the `cpp/InputLimits.h` byte limit (`maxJsonBytes`) is read live from
 *    native via [RlottieBridge.nativeGetInputLimits] (Chunk 5.1) instead of a
 *    hand-copied Kotlin constant, and enforced BEFORE a file/stream is read
 *    fully into memory — file-length checks happen before reads, and
 *    streamed copies (content/asset) abort as soon as the running byte count
 *    crosses the limit;
 *  - a missing source file is `SOURCE_NOT_FOUND`, never treated as empty;
 *  - raw animation JSON is never logged, including on the error paths below;
 *  - `resourcePath` (rlottie's external-asset root, Chunk 5.2) must be a
 *    dedicated per-animation-package app-private directory — never the bare
 *    shared root, never an arbitrary caller-supplied path — with every entry
 *    underneath it canonicalized, symlink-free, an allow-listed raster-image
 *    extension, and bounded by `maxExternalAssets` / `maxExternalBytes`.
 */
sealed class RlottieResolvedSource {
    /** Routes to `setSourceData(json, cacheKey, resourcePath)`. */
    data class Data(val json: String, val cacheKey: String, val resourcePath: String?) :
        RlottieResolvedSource()

    /** Routes to `setSourceFile(path)`; `path` is already canonical + sandbox-confined. */
    data class File(val path: String) : RlottieResolvedSource()

    /** Resolution failed; caller must surface this as `onAnimationError` verbatim. */
    data class Error(val code: String, val message: String) : RlottieResolvedSource()
}

/**
 * Chunk 5.1 — mirrors `rnrlottie::InputLimits` (`cpp/InputLimits.h`), read
 * live via [RlottieBridge.nativeGetInputLimits] rather than hand-copied into
 * Kotlin constants (the previous `MAX_SOURCE_BYTES = 16 MiB` literal this
 * chunk replaces was a documented drift hazard).
 */
private data class InputLimits(
    val maxJsonBytes: Long,
    val maxExternalAssets: Long,
    val maxExternalBytes: Long,
)

object RlottieSourceResolver {

    private const val CODE_INVALID_SOURCE = "INVALID_SOURCE"
    private const val CODE_SOURCE_NOT_FOUND = "SOURCE_NOT_FOUND"
    private const val CODE_SOURCE_TOO_LARGE = "SOURCE_TOO_LARGE"

    private const val COPY_DIR_NAME = "rlottie-sources"
    private const val COPY_BUFFER_BYTES = 64 * 1024

    // Raster-asset extensions rlottie's external-image path can actually
    // consume. v1 has no image-loader module built in (LOTTIE_MODULE OFF,
    // plan §14), but this allow-list is defense-in-depth for whatever raster
    // decoder a resource directory's files are eventually handed to, and
    // matches plan §12's "allow only known image extensions". png/jpg/jpeg
    // cover the formats Lottie/After Effects exporters actually emit for
    // embedded raster assets; webp is included because it's the other format
    // popular Lottie tooling commonly emits. Must stay identical to iOS's
    // `HasAllowedImageExtension` (RNRlottieSourceResolver.mm).
    private val ALLOWED_IMAGE_EXTENSIONS = setOf("png", "jpg", "jpeg", "webp")

    /** Chunk 5.1 — the live policy from `cpp/InputLimits.h`, fetched once per [resolve] call. */
    private fun currentLimits(): InputLimits {
        val values = RlottieBridge.nativeGetInputLimits()
        // The JNI contract (RlottieJni.cpp) always returns exactly
        // [maxJsonBytes, maxExternalAssets, maxExternalBytes]; both sides of
        // this JNI boundary are owned together, so an unexpected array shape
        // here would be a build-time contract break, not a runtime input to
        // guard against.
        return InputLimits(
            maxJsonBytes = values[0],
            maxExternalAssets = values[1],
            maxExternalBytes = values[2],
        )
    }

    fun resolve(context: Context, source: ReadableMap): RlottieResolvedSource {
        val limits = currentLimits()

        val json = stringOrNull(source, "json")
        if (json != null) {
            return resolveJson(context, source, json, limits)
        }

        val path = stringOrNull(source, "path")
        if (path != null) {
            return resolveFilePath(context, path, limits)
        }

        val uri = stringOrNull(source, "uri")
        if (uri != null) {
            return resolveUri(context, uri, limits)
        }

        return RlottieResolvedSource.Error(
            CODE_INVALID_SOURCE,
            "Unsupported `source` shape; expected one of {json}, {path}, {uri}.",
        )
    }

    // --- {json, cacheKey?, resourcePath?} ---------------------------------------

    private fun resolveJson(
        context: Context,
        source: ReadableMap,
        json: String,
        limits: InputLimits,
    ): RlottieResolvedSource {
        // Already fully materialized in memory by the bridge; still enforce the
        // limit for parity with the file-based paths below.
        val byteLength = json.toByteArray(Charsets.UTF_8).size.toLong()
        if (byteLength > limits.maxJsonBytes) {
            return RlottieResolvedSource.Error(
                CODE_SOURCE_TOO_LARGE,
                "animation JSON exceeds the configured byte limit",
            )
        }

        val cacheKey = stringOrNull(source, "cacheKey") ?: ""
        val rawResourcePath = stringOrNull(source, "resourcePath")
        val resourcePath = when {
            rawResourcePath.isNullOrEmpty() -> null
            else -> when (val result = validateResourceDirectory(context, rawResourcePath, limits)) {
                is ResourcePathResult.Ok -> result.path
                is ResourcePathResult.Fail ->
                    return RlottieResolvedSource.Error(result.code, result.message)
            }
        }
        return RlottieResolvedSource.Data(json, cacheKey, resourcePath)
    }

    // --- uri: file:// / content:// / asset:// -----------------------------------

    private fun resolveUri(context: Context, rawUri: String, limits: InputLimits): RlottieResolvedSource {
        val uri = try {
            Uri.parse(rawUri)
        } catch (_: Exception) {
            return RlottieResolvedSource.Error(CODE_INVALID_SOURCE, "malformed uri")
        }

        return when (val scheme = uri.scheme?.lowercase()) {
            "http", "https" ->
                RlottieResolvedSource.Error(
                    CODE_INVALID_SOURCE,
                    "remote sources are not supported in v1 (see plan §3, v1.1)",
                )

            "file" -> {
                val path = uri.path
                if (path.isNullOrEmpty()) {
                    RlottieResolvedSource.Error(CODE_INVALID_SOURCE, "file uri missing path")
                } else {
                    resolveFilePath(context, path, limits)
                }
            }

            "content" -> resolveContentUri(context, uri, limits)

            "asset" -> resolveAssetUri(context, uri, limits)

            null, "" ->
                RlottieResolvedSource.Error(CODE_INVALID_SOURCE, "uri missing scheme")

            else ->
                RlottieResolvedSource.Error(CODE_INVALID_SOURCE, "unsupported uri scheme: $scheme")
        }
    }

    private fun resolveContentUri(context: Context, uri: Uri, limits: InputLimits): RlottieResolvedSource {
        if (isRejectedContainer(uri.lastPathSegment)) {
            return RlottieResolvedSource.Error(
                CODE_INVALID_SOURCE,
                ".lottie containers are not supported in v1",
            )
        }
        val input = try {
            context.contentResolver.openInputStream(uri)
        } catch (_: Exception) {
            null
        } ?: return RlottieResolvedSource.Error(
            CODE_SOURCE_NOT_FOUND,
            "content uri could not be opened",
        )
        return copyToAppPrivateFile(context, input, "content", limits)
    }

    private fun resolveAssetUri(context: Context, uri: Uri, limits: InputLimits): RlottieResolvedSource {
        // `asset:///relative/path.json` — three slashes, empty authority; this
        // is the same shape RN itself uses for bundled non-image assets. The
        // relative path is the AssetManager entry name, not a real File, so it
        // is canonicalized as a string (no ".." segment, no absolute leading
        // component) rather than via File.canonicalFile.
        val rawAssetPath = uri.path?.removePrefix("/")
        if (rawAssetPath.isNullOrEmpty()) {
            return RlottieResolvedSource.Error(CODE_INVALID_SOURCE, "asset uri missing path")
        }
        if (isRejectedContainer(rawAssetPath)) {
            return RlottieResolvedSource.Error(
                CODE_INVALID_SOURCE,
                ".lottie containers are not supported in v1",
            )
        }
        val segments = rawAssetPath.split("/")
        if (segments.any { it.isEmpty() || it == ".." || it == "." }) {
            return RlottieResolvedSource.Error(CODE_INVALID_SOURCE, "invalid asset path")
        }

        val input: InputStream = try {
            context.assets.open(rawAssetPath)
        } catch (_: IOException) {
            return RlottieResolvedSource.Error(CODE_SOURCE_NOT_FOUND, "bundled asset not found")
        }
        return copyToAppPrivateFile(context, input, "asset", limits)
    }

    // --- {path}: an already-native filesystem path ------------------------------

    private fun resolveFilePath(context: Context, rawPath: String, limits: InputLimits): RlottieResolvedSource {
        if (isRejectedContainer(rawPath)) {
            return RlottieResolvedSource.Error(
                CODE_INVALID_SOURCE,
                ".lottie containers are not supported in v1",
            )
        }
        if (rawPath.contains("..")) {
            // Defense-in-depth ahead of canonicalization below, which is the
            // authoritative check — a literal ".." segment is rejected
            // outright, never silently resolved away.
            return RlottieResolvedSource.Error(CODE_INVALID_SOURCE, "path must not contain '..'")
        }

        val file = try {
            File(rawPath).canonicalFile
        } catch (_: IOException) {
            return RlottieResolvedSource.Error(CODE_INVALID_SOURCE, "unable to canonicalize path")
        }
        if (!isWithinAppPrivateDirectory(context, file)) {
            return RlottieResolvedSource.Error(
                CODE_INVALID_SOURCE,
                "path is outside the app-private directory",
            )
        }
        if (!file.exists() || !file.isFile) {
            return RlottieResolvedSource.Error(CODE_SOURCE_NOT_FOUND, "source file does not exist")
        }
        // Checked BEFORE any read of file contents.
        if (file.length() > limits.maxJsonBytes) {
            return RlottieResolvedSource.Error(
                CODE_SOURCE_TOO_LARGE,
                "source file exceeds the configured byte limit",
            )
        }
        return RlottieResolvedSource.File(file.path)
    }

    // --- content:// / asset:// -> app-private copy, byte-limited while streaming ---

    private fun copyToAppPrivateFile(
        context: Context,
        input: InputStream,
        prefix: String,
        limits: InputLimits,
    ): RlottieResolvedSource {
        val destDir = File(context.cacheDir, COPY_DIR_NAME)
        if (!destDir.exists() && !destDir.mkdirs() && !destDir.exists()) {
            input.closeQuietly()
            return RlottieResolvedSource.Error(
                CODE_SOURCE_NOT_FOUND,
                "unable to create app-private storage",
            )
        }
        val destFile = File(destDir, "$prefix-${System.nanoTime()}.json")

        input.use { stream ->
            var total = 0L
            try {
                FileOutputStream(destFile).use { out ->
                    val buffer = ByteArray(COPY_BUFFER_BYTES)
                    while (true) {
                        val read = stream.read(buffer)
                        if (read == -1) break
                        total += read
                        // Enforced WHILE copying, not after the fact: the limit
                        // check happens before the source is ever read fully
                        // into memory.
                        if (total > limits.maxJsonBytes) {
                            destFile.delete()
                            return RlottieResolvedSource.Error(
                                CODE_SOURCE_TOO_LARGE,
                                "source exceeds the configured byte limit",
                            )
                        }
                        out.write(buffer, 0, read)
                    }
                }
            } catch (_: IOException) {
                destFile.delete()
                return RlottieResolvedSource.Error(CODE_SOURCE_NOT_FOUND, "failed reading source")
            }
        }

        // Re-run the same canonicalization + confinement check used for every
        // other path, even though this file was just written under cacheDir —
        // one code path for "is this path safe to hand to setSourceFile", no
        // special-cased trust for the copy we just made.
        return resolveFilePath(context, destFile.path, limits)
    }

    private fun InputStream.closeQuietly() {
        try {
            close()
        } catch (_: IOException) {
            // ignored
        }
    }

    // --- Sandbox confinement ------------------------------------------------------

    private fun appPrivateRoots(context: Context): List<File> {
        val candidates = listOfNotNull(
            context.filesDir,
            context.cacheDir,
            context.noBackupFilesDir,
        )
        return candidates.mapNotNull { root ->
            try {
                root.canonicalFile
            } catch (_: IOException) {
                null
            }
        }
    }

    private fun isWithinAppPrivateDirectory(context: Context, canonical: File): Boolean {
        val candidatePath = canonical.path
        return appPrivateRoots(context).any { root ->
            candidatePath == root.path || candidatePath.startsWith(root.path + File.separator)
        }
    }

    // --- resourcePath (Chunk 5.2): dedicated per-package asset directory ---------

    private sealed class ResourcePathResult {
        data class Ok(val path: String) : ResourcePathResult()
        data class Fail(val code: String, val message: String) : ResourcePathResult()
    }

    /**
     * Validates `rawPath` as rlottie's external-asset `resourcePath` root
     * (plan §12/§16, Chunk 5.2):
     *  - canonicalized, confined to an app-private directory, and NOT one of
     *    those shared roots itself — it must be a dedicated per-animation-
     *    package directory ("create one private directory per animation
     *    package"), so two unrelated animations can't share (and read each
     *    other's) asset storage;
     *  - every entry under it, recursively: no symlinks (a conservative
     *    superset of "reject symlinks that escape the directory" — nothing
     *    but the caller's own raster assets is ever legitimate here, so no
     *    symlink is ever legitimate either); regular files only, with an
     *    allow-listed image extension;
     *  - bounded file count and total bytes ([InputLimits.maxExternalAssets] /
     *    [InputLimits.maxExternalBytes]).
     * Mirrors iOS's `ValidateResourceDirectory` (RNRlottieSourceResolver.mm)
     * check-for-check.
     */
    private fun validateResourceDirectory(
        context: Context,
        rawPath: String,
        limits: InputLimits,
    ): ResourcePathResult {
        if (rawPath.contains("..")) {
            return ResourcePathResult.Fail(CODE_INVALID_SOURCE, "resourcePath must not contain '..'")
        }
        val dir = try {
            File(rawPath).canonicalFile
        } catch (_: IOException) {
            return ResourcePathResult.Fail(CODE_INVALID_SOURCE, "unable to canonicalize resourcePath")
        }
        if (!isWithinAppPrivateDirectory(context, dir)) {
            return ResourcePathResult.Fail(
                CODE_INVALID_SOURCE,
                "resourcePath is outside the app-private directory",
            )
        }
        if (appPrivateRoots(context).any { it.path == dir.path }) {
            return ResourcePathResult.Fail(
                CODE_INVALID_SOURCE,
                "resourcePath must be a dedicated per-animation directory, not a shared root",
            )
        }
        if (!dir.exists()) {
            return ResourcePathResult.Fail(CODE_SOURCE_NOT_FOUND, "resourcePath directory does not exist")
        }
        if (!dir.isDirectory) {
            return ResourcePathResult.Fail(CODE_INVALID_SOURCE, "resourcePath is not a directory")
        }

        var fileCount = 0L
        var totalBytes = 0L

        fun walk(node: File): ResourcePathResult.Fail? {
            val children = node.listFiles() ?: return null
            for (child in children) {
                if (Files.isSymbolicLink(child.toPath())) {
                    return ResourcePathResult.Fail(
                        CODE_INVALID_SOURCE,
                        "resourcePath must not contain symlinks",
                    )
                }
                if (child.isDirectory) {
                    val nested = walk(child)
                    if (nested != null) return nested
                    continue
                }
                if (!child.isFile) {
                    return ResourcePathResult.Fail(
                        CODE_INVALID_SOURCE,
                        "resourcePath must contain only regular files",
                    )
                }
                if (child.extension.lowercase() !in ALLOWED_IMAGE_EXTENSIONS) {
                    return ResourcePathResult.Fail(
                        CODE_INVALID_SOURCE,
                        "resourcePath may only contain png/jpg/jpeg/webp assets",
                    )
                }

                fileCount += 1
                if (fileCount > limits.maxExternalAssets) {
                    return ResourcePathResult.Fail(
                        CODE_SOURCE_TOO_LARGE,
                        "resourcePath exceeds the configured asset-count limit",
                    )
                }

                totalBytes += child.length()
                if (totalBytes > limits.maxExternalBytes) {
                    return ResourcePathResult.Fail(
                        CODE_SOURCE_TOO_LARGE,
                        "resourcePath exceeds the configured total-bytes limit",
                    )
                }
            }
            return null
        }

        val failure = walk(dir)
        if (failure != null) return failure
        return ResourcePathResult.Ok(dir.path)
    }

    // --- Shared helpers -----------------------------------------------------------

    private fun isRejectedContainer(pathOrName: String?): Boolean =
        pathOrName?.lowercase()?.endsWith(".lottie") == true

    private fun stringOrNull(map: ReadableMap, key: String): String? {
        if (!map.hasKey(key) || map.isNull(key)) return null
        return map.getString(key)
    }
}
