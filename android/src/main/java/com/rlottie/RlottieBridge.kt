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

    @JvmStatic external fun nativePlay(h: Long)
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
}
