package com.rlottie

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Rect
import android.util.AttributeSet
import android.view.Choreographer
import android.view.View
import kotlin.math.max
import kotlin.math.sqrt

/**
 * Chunk 3.2 — the custom `View` + `Choreographer` presenter that drives the
 * frozen JNI surface in `RlottieBridge`/`RlottieJni.cpp` (Chunk 3.1).
 *
 * This class is the ONLY thing in the Android adapter that talks to
 * `RlottieBridge`. `RlottieViewManager` (Chunk 3.3) drives it purely through
 * the public Kotlin methods and listener properties below — it never touches
 * `UIManager`, JNI, or the native handle itself. That mirrors the TS-facing
 * rule in CLAUDE.md ("consumers never touch platform methods") one layer
 * down: the ViewManager is this view's only consumer, and it stays thin.
 *
 * ── The per-tick poll/drain/present sequence ────────────────────────────
 *
 * `AndroidFrameSink` (Chunk 3.1) is deliberately poll-based: the render
 * worker thread never calls into the JVM, so there is no JavaVM attach/detach
 * on that thread and no lifetime hazard from a worker->Java callback outliving
 * the view. Instead the worker only flips atomics / pushes to a mutex-guarded
 * queue, and the UI thread drains that state once per display tick, inside
 * the `Choreographer.FrameCallback`:
 *
 *   1. `nativeAdvance(handle, monotonicSeconds)` — runs the playback state
 *      machine for this tick (pure logic, [UI]-thread-owned per
 *      PlaybackController's contract) and, if a new frame is due, requests it
 *      from the render worker. Returns a `PlaybackEvent` ordinal that this
 *      view maps to a lifecycle listener call.
 *   2. `nativePollEvent(handle)` — drained in a loop (not just once) because
 *      the sink's event queue can hold more than one event per tick (e.g. a
 *      fast-loading source can queue `loaded` and then immediately an error
 *      from a bad asset reference). Loop until null.
 *   3. `nativeHasNewFrame` / `nativeCopyFrontInto` — the render worker may
 *      have published a new frame asynchronously since the last tick (it runs
 *      on its own thread, decoupled from the display clock). If one is ready,
 *      copy it into the back `Bitmap` and only THEN swap+invalidate — see
 *      below for why the swap, not the copy, is the presentation boundary.
 *
 * Order matters: advance (which may request a render) is issued before we
 * check for a completed frame, so a frame requested on a previous tick has
 * had a full tick's worth of worker time to complete before we look for it.
 * This is a single-in-flight, latest-frame-wins pipeline by construction —
 * `nativeAdvance`/`RenderCoordinator` never let more than one render queue.
 *
 * ── Double-buffer / bitmap-swap rationale ───────────────────────────────
 *
 * Two `ARGB_8888` Bitmaps (`frontBitmap`, `backBitmap`) mirror the native
 * front/back `FrameBuffer` (plan §5/§9): `onDraw` only ever reads
 * `frontBitmap`, and only `nativeCopyFrontInto` ever writes into
 * `backBitmap`. Both happen on the UI thread, so there is no cross-thread
 * race here — the swap exists so `onDraw` never observes a bitmap mid-write
 * (e.g. if a future change made the copy span >1 tick) and so a resize can
 * reallocate the *back* bitmap without ever touching the one currently being
 * drawn.
 *
 * Resize handling piggybacks on the same swap: `onSizeChanged` only computes
 * and debounces a target pixel size and forwards it to
 * `nativeSetSurfaceSize` — it never touches the Bitmaps. `backBitmap` is
 * lazily reallocated to the latest requested size right before the next copy
 * attempt (see `ensureBackBitmapSized`); `frontBitmap` is left completely
 * alone until a copy into the newly-sized back buffer actually succeeds and
 * we swap. That means: (a) the currently displayed frame never blanks out
 * mid-resize — `onDraw` keeps drawing the old `frontBitmap` (Canvas scales it
 * to the view's current bounds regardless of the bitmap's own pixel size), and
 * (b) `nativeCopyFrontInto` returning false because the native front buffer
 * hasn't caught up to the new size yet is expected and silently retried next
 * tick, not an error.
 */
class RlottieView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
    maxPixels: Long = DEFAULT_MAX_PIXELS,
) : View(context, attrs, defStyleAttr) {

    // --- Native handle -------------------------------------------------------
    //
    // 0L means "no live handle" — either never created (OOM in nativeCreate)
    // or already released. Every native call in this class is guarded by
    // `handle != 0L`; `release()` zeroes it BEFORE calling nativeDestroy so a
    // reentrant/duplicate call (or a Choreographer tick racing teardown — it
    // can't, both are UI-thread-only, but the guard is cheap defense-in-depth)
    // can never call a native method after destroy.
    private var handle: Long = RlottieBridge.nativeCreate(maxPixels)

    // --- Presentation: double-buffered ARGB_8888 bitmaps ----------------------

    private var frontBitmap: Bitmap? = null
    private var backBitmap: Bitmap? = null

    private val drawRect = Rect()
    private val drawPaint = Paint(Paint.FILTER_BITMAP_FLAG or Paint.ANTI_ALIAS_FLAG)

    // --- Resize: computed target size, debounced before it reaches native -----

    /** Multiplies the density-scaled layout size before clamping to the pixel budget. */
    var renderScale: Float = 1f
        set(value) {
            val clamped = value.coerceIn(MIN_RENDER_SCALE, MAX_RENDER_SCALE)
            if (clamped == field) return
            field = clamped
            scheduleSurfaceResize()
        }

    /** The render-area budget (pixels) used to clamp resize targets; mirrors nativeCreate's. */
    private val maxPixelArea: Long = if (maxPixels > 0) maxPixels else DEFAULT_MAX_PIXELS

    private var pendingPixelW = 0
    private var pendingPixelH = 0
    private val resizeRunnable = Runnable { applyPendingSurfaceSize() }

    // --- Choreographer lifecycle -----------------------------------------------

    private var choreographerRunning = false
    private val frameCallback = Choreographer.FrameCallback { frameTimeNanos ->
        onFrameTick(frameTimeNanos)
    }

    // --- Listener surface for Chunk 3.3 -----------------------------------------
    //
    // RlottieViewManager wires these to RN direct events; this view knows
    // nothing about WritableMap/RCTEventEmitter. Each is invoked synchronously
    // from the Choreographer tick (UI thread), matching where RN expects
    // events to originate.

    /** Fired once per successful `loaded` event (source parsed, metadata known). */
    var onAnimationLoaded: ((RlottieLoadedInfo) -> Unit)? = null

    /** Fired for a load failure OR a runtime render failure surfaced via the event queue. */
    var onAnimationFailure: ((RlottiePlayerError) -> Unit)? = null

    /** PlaybackEvent.Started */
    var onAnimationStart: (() -> Unit)? = null

    /** PlaybackEvent.Paused (reaching a paused state during a tick, not the play() call itself) */
    var onAnimationPause: (() -> Unit)? = null

    /** PlaybackEvent.Loop (one loop wrap completed) */
    var onAnimationLoop: (() -> Unit)? = null

    /** PlaybackEvent.Finished */
    var onAnimationFinish: (() -> Unit)? = null

    // --- Public imperative API ---------------------------------------------------
    //
    // Every method below is a thin, guarded forward to RlottieBridge. None of
    // them do anything once `handle == 0L`; RlottieViewManager and the future
    // TS command layer can call them freely before/after mount/unmount without
    // special-casing.

    fun setSourceData(json: String, cacheKey: String, resourcePath: String?) {
        val h = handle
        if (h == 0L) return
        RlottieBridge.nativeSetSourceData(h, json, cacheKey, resourcePath)
    }

    fun setSourceFile(path: String) {
        val h = handle
        if (h == 0L) return
        RlottieBridge.nativeSetSourceFile(h, path)
    }

    fun configure(
        loop: Boolean,
        repeatCount: Int,
        speed: Double,
        startFrame: Int,
        endFrame: Int,
        autoPlay: Boolean,
    ) {
        val h = handle
        if (h == 0L) return
        RlottieBridge.nativeConfigure(h, loop, repeatCount, speed, startFrame, endFrame, autoPlay)
    }

    /**
     * [startFrame]/[endFrame] override the played segment for THIS play only
     * (-1 = unset); they do not change the range configured via [configure].
     * Mirrors iOS's `-playFromFrame:toFrame:` so both platforms behave
     * identically for the contract's `play` command.
     */
    @JvmOverloads
    fun play(startFrame: Int = NO_FRAME_OVERRIDE, endFrame: Int = NO_FRAME_OVERRIDE) {
        val h = handle
        if (h != 0L) RlottieBridge.nativePlay(h, startFrame, endFrame)
    }

    fun pause() {
        val h = handle
        if (h != 0L) RlottieBridge.nativePause(h)
    }

    fun resume() {
        val h = handle
        if (h != 0L) RlottieBridge.nativeResume(h)
    }

    fun stop() {
        val h = handle
        if (h != 0L) RlottieBridge.nativeStop(h)
    }

    fun reset() {
        val h = handle
        if (h != 0L) RlottieBridge.nativeReset(h)
    }

    fun seekToFrame(frame: Int) {
        val h = handle
        if (h != 0L) RlottieBridge.nativeSeekFrame(h, frame)
    }

    fun seekToProgress(progress: Double) {
        val h = handle
        if (h != 0L) RlottieBridge.nativeSeekProgress(h, progress)
    }

    fun setSpeed(speed: Double) {
        val h = handle
        if (h != 0L) RlottieBridge.nativeSetSpeed(h, speed)
    }

    fun setColor(keyPath: String, r: Float, g: Float, b: Float) {
        val h = handle
        if (h != 0L) RlottieBridge.nativeSetColor(h, keyPath, r, g, b)
    }

    /**
     * Stops the Choreographer callback, destroys the native handle (idempotent
     * there too), zeroes it, and frees the Bitmaps. Safe to call twice — the
     * `handle == 0L` guard makes every subsequent call (from here or from any
     * of the methods above) a no-op. Chunk 3.3's `onDropViewInstance` calls
     * this.
     */
    fun release() {
        pauseChoreographer()
        removeCallbacks(resizeRunnable)
        val h = handle
        if (h == 0L) return
        handle = 0L
        RlottieBridge.nativeDestroy(h)
        recycleBitmaps()
    }

    // --- View lifecycle -----------------------------------------------------------

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        resumeChoreographer()
    }

    override fun onDetachedFromWindow() {
        pauseChoreographer()
        super.onDetachedFromWindow()
    }

    override fun onWindowVisibilityChanged(visibility: Int) {
        super.onWindowVisibilityChanged(visibility)
        if (visibility == VISIBLE) {
            resumeChoreographer()
        } else {
            pauseChoreographer()
        }
    }

    /**
     * Suspends the Choreographer clock without touching playback state (no
     * `nativePause`/anchor reset — `nativeAdvance`'s anchor math already
     * tolerates a gap in ticks). This view is a plain `android.view.View`
     * with no `ReactContext` reference by design, so it cannot register its
     * own `LifecycleEventListener`; Chunk 3.3's ViewManager/host listener
     * calls this on `onHostPause`. Safe to call when already paused.
     */
    fun onHostPause() {
        pauseChoreographer()
    }

    /** Counterpart to [onHostPause]; Chunk 3.3 calls this from `onHostResume`. */
    fun onHostResume() {
        resumeChoreographer()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        scheduleSurfaceResize()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val bmp = frontBitmap ?: return
        if (bmp.isRecycled) return
        drawRect.set(0, 0, width, height)
        // The bitmap's own pixel dimensions need not match the view's current
        // size: Canvas scales src->dst, which is exactly what lets us keep
        // showing an old-sized frontBitmap across a resize without blanking.
        canvas.drawBitmap(bmp, null, drawRect, drawPaint)
    }

    // --- Choreographer tick -------------------------------------------------------

    private fun resumeChoreographer() {
        if (choreographerRunning || handle == 0L || !isAttachedToWindow) return
        choreographerRunning = true
        Choreographer.getInstance().postFrameCallback(frameCallback)
    }

    private fun pauseChoreographer() {
        if (!choreographerRunning) return
        choreographerRunning = false
        Choreographer.getInstance().removeFrameCallback(frameCallback)
    }

    private fun onFrameTick(frameTimeNanos: Long) {
        // Re-post before doing any work so a listener throwing, or an early
        // return below, can never silently stop the animation.
        if (!choreographerRunning) return
        Choreographer.getInstance().postFrameCallback(frameCallback)

        val h = handle
        if (h == 0L) return

        val monotonicSeconds = frameTimeNanos / 1_000_000_000.0
        val eventOrdinal = RlottieBridge.nativeAdvance(h, monotonicSeconds)
        dispatchPlaybackEvent(eventOrdinal)

        drainEvents(h)
        presentIfNewFrame(h)
    }

    private fun dispatchPlaybackEvent(ordinal: Int) {
        // 0=None, 1=Started, 2=Paused, 3=Loop, 4=Finished — see RlottieJni.cpp.
        when (ordinal) {
            1 -> onAnimationStart?.invoke()
            2 -> onAnimationPause?.invoke()
            3 -> onAnimationLoop?.invoke()
            4 -> onAnimationFinish?.invoke()
        }
    }

    private fun drainEvents(h: Long) {
        // Loop, not a single poll: the sink's queue (SinkState, bounded to 64)
        // can hold more than one event per tick.
        while (true) {
            val encoded = RlottieBridge.nativePollEvent(h) ?: break
            when (val event = parseEvent(encoded)) {
                is NativeEvent.Loaded -> onAnimationLoaded?.invoke(event.info)
                is NativeEvent.Error -> onAnimationFailure?.invoke(event.error)
                null -> Unit // Unrecognized wire format; drop rather than crash.
            }
        }
    }

    private fun presentIfNewFrame(h: Long) {
        if (!RlottieBridge.nativeHasNewFrame(h)) return
        if (pendingPixelW <= 0 || pendingPixelH <= 0) return

        ensureBackBitmapSized()
        val back = backBitmap ?: return

        if (RlottieBridge.nativeCopyFrontInto(h, back)) {
            val previousFront = frontBitmap
            frontBitmap = back
            backBitmap = previousFront
            invalidate()
        }
        // false => the native front buffer doesn't match `back`'s dimensions
        // yet (normal right after a resize) or there was nothing to copy;
        // frontBitmap (and thus onDraw) is untouched, so the last valid frame
        // keeps showing. Retried next tick.
    }

    private fun ensureBackBitmapSized() {
        // Bitmap carries its own width/height, so comparing against the
        // *current* backBitmap's own dimensions (rather than a separately
        // tracked field) stays correct across a front/back swap: whichever
        // Bitmap object lands in `backBitmap` is checked against what it
        // actually is, not against stale bookkeeping from before the swap.
        val current = backBitmap
        if (current != null && !current.isRecycled &&
            current.width == pendingPixelW && current.height == pendingPixelH
        ) {
            return
        }
        current?.let { if (!it.isRecycled) it.recycle() }
        backBitmap = try {
            Bitmap.createBitmap(pendingPixelW, pendingPixelH, Bitmap.Config.ARGB_8888)
        } catch (_: OutOfMemoryError) {
            null
        } catch (_: IllegalArgumentException) {
            null
        }
    }

    private fun recycleBitmaps() {
        frontBitmap?.let { if (!it.isRecycled) it.recycle() }
        backBitmap?.let { if (!it.isRecycled) it.recycle() }
        frontBitmap = null
        backBitmap = null
    }

    // --- Resize: debounce + clamp ------------------------------------------------

    private fun scheduleSurfaceResize() {
        removeCallbacks(resizeRunnable)
        if (width <= 0 || height <= 0 || handle == 0L) return
        postDelayed(resizeRunnable, RESIZE_DEBOUNCE_MS)
    }

    private fun applyPendingSurfaceSize() {
        val h = handle
        if (h == 0L || width <= 0 || height <= 0) return

        // View.getWidth()/getHeight() are ALREADY physical pixels — the plan's
        // "dp * density" is the dp->px conversion, which the View has already
        // done for us. Multiplying by density here would double-count it and
        // request ~density^2 times the pixels (9x on an xxhdpi screen), burning
        // render time and hitting the area clamp far too early. renderScale is
        // the only multiplier that belongs here. (iOS differs: UIView bounds are
        // in points, so RNRlottieView legitimately multiplies by screenScale.)
        var pxW = max(1.0, width.toDouble() * renderScale).toLong()
        var pxH = max(1.0, height.toDouble() * renderScale).toLong()

        val area = pxW * pxH
        if (area > maxPixelArea) {
            val scale = sqrt(maxPixelArea.toDouble() / area.toDouble())
            pxW = max(1L, (pxW * scale).toLong())
            pxH = max(1L, (pxH * scale).toLong())
        }

        val newW = pxW.coerceAtMost(Int.MAX_VALUE.toLong()).toInt()
        val newH = pxH.coerceAtMost(Int.MAX_VALUE.toLong()).toInt()
        if (newW == pendingPixelW && newH == pendingPixelH) return

        pendingPixelW = newW
        pendingPixelH = newH
        RlottieBridge.nativeSetSurfaceSize(h, pendingPixelW, pendingPixelH)
    }

    // --- Event wire-format parsing (mirrors AndroidEventEncoding.h) ---------------

    private sealed class NativeEvent {
        data class Loaded(val info: RlottieLoadedInfo) : NativeEvent()
        data class Error(val error: RlottiePlayerError) : NativeEvent()
    }

    private fun parseEvent(encoded: String): NativeEvent? = when {
        encoded.startsWith("loaded:") -> parseLoaded(encoded.substring(LOADED_PREFIX_LEN))
        encoded.startsWith("error:") -> parseError(encoded)
        else -> null
    }

    private fun parseError(encoded: String): NativeEvent.Error? {
        // "error:<CODE>:<message>" — split with limit 3; the message is the
        // tail and may itself contain further ':' characters.
        val parts = encoded.split(":", limit = 3)
        if (parts.size < 3) return null
        return NativeEvent.Error(RlottiePlayerError(code = parts[1], message = parts[2]))
    }

    private fun parseLoaded(payload: String): NativeEvent.Loaded? {
        val fields = payload.split(",")
        if (fields.size < 6) return null
        return try {
            val width = fields[0].toInt()
            val height = fields[1].toInt()
            val frameRate = fields[2].toDouble()
            val totalFrames = fields[3].toInt()
            val duration = fields[4].toDouble()
            val markerCount = fields[5].toInt()

            val markers = ArrayList<RlottieMarker>(max(0, markerCount))
            var idx = 6
            var remaining = markerCount
            while (remaining > 0 && idx + 2 < fields.size) {
                val name = fields[idx]
                val start = fields[idx + 1].toIntOrNull() ?: 0
                val end = fields[idx + 2].toIntOrNull() ?: 0
                markers.add(RlottieMarker(name, start, end))
                idx += 3
                remaining -= 1
            }

            NativeEvent.Loaded(
                RlottieLoadedInfo(
                    width = width,
                    height = height,
                    frameRate = frameRate,
                    totalFrames = totalFrames,
                    duration = duration,
                    markers = markers,
                ),
            )
        } catch (_: NumberFormatException) {
            null
        }
    }

    companion object {
        // Matches cpp/FrameBuffer.h's default `maxPixels = 4096 * 4096` so a
        // view created without an explicit budget behaves like the native
        // default. Chunk 3.4's config module can thread a smaller/larger
        // budget through the constructor once it exists.
        const val DEFAULT_MAX_PIXELS: Long = 4096L * 4096L

        /** Sentinel for [play]'s optional segment override (docs/bridge-contract.md). */
        const val NO_FRAME_OVERRIDE: Int = -1

        private const val RESIZE_DEBOUNCE_MS: Long = 80L
        private const val MIN_RENDER_SCALE = 0.05f
        private const val MAX_RENDER_SCALE = 4f
        private const val LOADED_PREFIX_LEN = 7 // "loaded:".length
    }
}

/** A named frame segment from the Lottie source (marker names are pre-sanitized natively). */
data class RlottieMarker(val name: String, val startFrame: Int, val endFrame: Int)

/** Mirrors cpp/AnimationMetadata.h, decoded from the `loaded:` wire event. */
data class RlottieLoadedInfo(
    val width: Int,
    val height: Int,
    val frameRate: Double,
    val totalFrames: Int,
    val duration: Double,
    val markers: List<RlottieMarker>,
)

/** `code` is one of the canonical strings in cpp/ErrorCode.h (identical on iOS/Android). */
data class RlottiePlayerError(val code: String, val message: String)
