package com.rlottie

import android.graphics.Bitmap

/**
 * Chunk 3.2 — the Kotlin half of the frozen JNI surface exported by
 * `android/src/main/cpp/RlottieJni.cpp` (Chunk 3.1; do not edit that file or
 * add/rename/retype anything here without changing the C++ side in lockstep).
 *
 * `RlottieBridge` is a Kotlin `object`, and a plain `external fun` on an
 * object compiles to an *instance* method on the singleton (called through
 * `RlottieBridge.INSTANCE` from native's point of view) — that does not match
 * the `Java_com_rlottie_RlottieBridge_<name>` static symbols the JNI file
 * exports. `@JvmStatic` forces true static JNI methods so the linker finds
 * them.
 *
 * Threading contract (see the header comment in RlottieJni.cpp — repeated
 * here because callers of this object must honor it):
 *   * Every function that takes a handle must be called from the UI thread
 *     only. `RlottieView` (the sole caller) enforces this by construction:
 *     it only ever calls these from its own methods and its
 *     `Choreographer.FrameCallback`, both of which run on the UI thread.
 *   * `nativeDestroy` must be called at most once per handle; the handle
 *     value must never be reused afterward. `RlottieView` enforces this by
 *     zeroing its `handle` field before calling `nativeDestroy` and guarding
 *     every other call with `handle != 0L`.
 *
 * The `Long` handle is NOT a pointer — it is an opaque id into a native
 * registry (see `JniPlayerHandle.h`). A stale or forged id fails safe on the
 * native side, but callers should still never call anything after
 * `nativeDestroy` — see `RlottieView.release()`.
 */
internal object RlottieBridge {
    init {
        System.loadLibrary("react-native-rlottie")
    }

    // --- Lifecycle --------------------------------------------------------

    /** Returns 0 on failure. `maxPixels` bounds the render-area / buffer budget. */
    @JvmStatic external fun nativeCreate(maxPixels: Long): Long

    /** Idempotent; a zero/stale/already-destroyed handle is a no-op. */
    @JvmStatic external fun nativeDestroy(h: Long)

    // --- Source -------------------------------------------------------------

    @JvmStatic
    external fun nativeSetSourceData(h: Long, json: String, cacheKey: String, resourcePath: String?)

    @JvmStatic external fun nativeSetSourceFile(h: Long, path: String)

    // --- Surface / configuration -------------------------------------------

    @JvmStatic external fun nativeSetSurfaceSize(h: Long, w: Int, hgt: Int)

    @JvmStatic
    external fun nativeConfigure(
        h: Long,
        loop: Boolean,
        repeatCount: Int,
        speed: Double,
        startFrame: Int,
        endFrame: Int,
        autoPlay: Boolean,
    )

    // --- Transport commands --------------------------------------------------

    /**
     * `startFrame`/`endFrame` are a ONE-OFF segment override for this play only;
     * -1 means "unset". They do not mutate the persistent config set by
     * [nativeConfigure] — see the comment on the C++ side in RlottieJni.cpp.
     */
    @JvmStatic external fun nativePlay(h: Long, startFrame: Int, endFrame: Int)
    @JvmStatic external fun nativePause(h: Long)
    @JvmStatic external fun nativeResume(h: Long)
    @JvmStatic external fun nativeStop(h: Long)
    @JvmStatic external fun nativeReset(h: Long)
    @JvmStatic external fun nativeSeekFrame(h: Long, frame: Int)
    @JvmStatic external fun nativeSeekProgress(h: Long, progress: Double)
    @JvmStatic external fun nativeSetSpeed(h: Long, speed: Double)

    // --- Per-tick driving + presentation -------------------------------------

    /** Advances playback and requests a render; returns a PlaybackEvent ordinal (0..4). */
    @JvmStatic external fun nativeAdvance(h: Long, monotonicSeconds: Double): Int

    @JvmStatic external fun nativeHasNewFrame(h: Long): Boolean

    /**
     * Copies the native front buffer into `bitmap` (must be `ARGB_8888` and
     * exactly the native front buffer's pixel dimensions). Returns false
     * without side effects when there is no frame or the dimensions don't
     * match yet — the normal transient state right after a resize.
     */
    @JvmStatic external fun nativeCopyFrontInto(h: Long, bitmap: Bitmap): Boolean

    /** Pops one queued load/error event (encoded per AndroidEventEncoding.h), or null. */
    @JvmStatic external fun nativePollEvent(h: Long): String?

    // --- Dynamic properties ---------------------------------------------------

    @JvmStatic external fun nativeSetColor(h: Long, keyPath: String, r: Float, g: Float, b: Float)

    // --- Phase 6: dynamic properties + playMarker (docs/bridge-contract.md's
    // "Phase 6 additions") ---------------------------------------------------

    /** Backs `opacityOverrides`; opacity 0..1, clamped natively out of range. */
    @JvmStatic external fun nativeSetOpacity(h: Long, keyPath: String, opacity: Float)

    /** Backs `strokeWidthOverrides`; width in px, clamped natively out of range. */
    @JvmStatic external fun nativeSetStrokeWidth(h: Long, keyPath: String, width: Float)

    /**
     * Backs command id 9 (`playMarker`). [UI]-only, same threading contract as
     * [nativePlay]/[nativePause]/etc. above. Returns false for an unknown
     * marker name — a silent no-op, not an error/event.
     */
    @JvmStatic external fun nativePlayMarker(h: Long, name: String): Boolean

    // --- Phase 7: instrumentation (docs/bridge-contract.md "Phase 7
    // additions") -------------------------------------------------------------

    /**
     * Gates RenderCoordinator's own metrics collection; thread-safe on the
     * coordinator, same as [nativeSetColor]. Default false natively.
     */
    @JvmStatic external fun nativeSetMetricsEnabled(h: Long, enabled: Boolean)

    /**
     * A snapshot of `rnrlottie::MetricsSnapshot` (cpp/Metrics.h), encoded as 9
     * doubles in a fixed order — the mixed double/uint64 struct fields all fit
     * losslessly in a JS-safe double (frame/alloc/byte counts here are nowhere
     * near 2^53), so one flat array keeps this JNI surface as narrow as
     * [nativeGetInputLimits]'s LongArray convention:
     *
     *   [parseMs, firstFrameMs, renderP50Ms, renderP95Ms, renderP99Ms,
     *    framesRendered, framesDropped, bufferAllocCount, peakBufferBytes]
     *
     * Returns a zero-length array for an invalid/stale handle (never null) so
     * [RlottieView] can check `size < 9` instead of null-checking.
     */
    @JvmStatic external fun nativeGetMetricsSnapshot(h: Long): DoubleArray

    // --- Global / process-wide (Chunk 3.4) -------------------------------------
    //
    // No handle: these back `ModelCacheController`, process-global rlottie
    // model-cache state, never per-view playback (plan §15). Called only from
    // [RlottieModule], not from [RlottieView].

    @JvmStatic external fun nativeSetModelCacheSize(entries: Long)
    @JvmStatic external fun nativeClearModelCache()
    @JvmStatic external fun nativeGetModelCacheSize(): Long
    @JvmStatic external fun nativeGetRlottieCommit(): String

    /**
     * Chunk 5.1 — the live `cpp/InputLimits.h` policy, as
     * `[maxJsonBytes, maxExternalAssets, maxExternalBytes]`. [RlottieSourceResolver]
     * reads this instead of hand-copying the C++ defaults into a Kotlin
     * constant, so the two can no longer drift apart silently. No handle:
     * global input-validation policy, not per-view state.
     */
    @JvmStatic external fun nativeGetInputLimits(): LongArray
}
