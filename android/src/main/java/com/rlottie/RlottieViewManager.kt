package com.rlottie

import com.facebook.react.bridge.LifecycleEventListener
import com.facebook.react.bridge.ReadableArray
import com.facebook.react.bridge.ReadableMap
import com.facebook.react.common.MapBuilder
import com.facebook.react.uimanager.SimpleViewManager
import com.facebook.react.uimanager.ThemedReactContext
import com.facebook.react.uimanager.annotations.ReactProp
import com.facebook.react.uimanager.events.RCTEventEmitter
import java.util.WeakHashMap

/**
 * Chunk 3.3 — the `SimpleViewManager` half of the frozen bridge contract
 * (`docs/bridge-contract.md`), which this file must match byte-for-byte with
 * `ios/RNRlottieViewManager.mm`: prop names/defaults, the eight numeric
 * command ids, and the six direct-event names/payload shapes.
 *
 * This class is deliberately thin: it owns no playback logic, only
 * prop/command/event plumbing onto [RlottieView] (Chunk 3.2) and the
 * `WritableMap` conversions delegated to [RlottieEvents]. `RlottieView`
 * itself never touches RN types, per CLAUDE.md's layering rule one level up
 * (consumers never touch `UIManager`/platform methods — here, the analogous
 * rule is "the native view never touches RN bridge types").
 *
 * ── Per-view state (`WeakHashMap`, not manager fields) ──────────────────
 *
 * A `ViewManager` instance is a *singleton* shared by every `RlottieView` RN
 * mounts, so anything that varies per-instance (the coalesced playback
 * config, the `pauseWhenInactive` flag, the registered `LifecycleEventListener`)
 * cannot live in manager fields — it is keyed by view identity in
 * [viewStates], which is torn down per-view in [onDropViewInstance].
 *
 * ── Coalescing the playback-shaping props into one `configure()` call ────
 *
 * `loop`, `repeatCount`, `speed`, `startFrame`, `endFrame`, and `autoPlay` all
 * map onto the single native `RlottieView.configure(...)` call. `@ReactProp`
 * setters fire one prop at a time as RN diffs the props object, so applying
 * each directly would make a partially-applied config briefly live (e.g.
 * `loop` flips before `repeatCount` catches up). Instead each setter only
 * mutates the view's [PlaybackConfig] and marks it dirty; [onAfterUpdateTransaction]
 * — RN's "all `@ReactProp` setters for this transaction have now run" hook —
 * flushes exactly one `configure()` call per transaction, only if something
 * playback-shaping actually changed.
 */
class RlottieViewManager : SimpleViewManager<RlottieView>() {

    /** Per-view mutable state; entries are removed explicitly in [onDropViewInstance]. */
    private class ViewState {
        val config = PlaybackConfig()
        var pauseWhenInactive: Boolean = true
        var lifecycleListener: LifecycleEventListener? = null
    }

    private class PlaybackConfig {
        var loop: Boolean = false
        var repeatCount: Int = 0
        var speed: Double = 1.0
        var startFrame: Int = 0
        var endFrame: Int = 0
        var autoPlay: Boolean = false

        // Starts dirty so the very first `onAfterUpdateTransaction` after mount
        // always flushes one `configure()` call, even if every playback-shaping
        // prop is left at its default (i.e. never touched by an @ReactProp
        // setter) — this keeps native state and this class's bookkeeping in
        // sync from the start rather than relying on native's own defaults
        // happening to match the contract's documented defaults.
        var dirty: Boolean = true
    }

    // Identity-keyed (RlottieView has no custom equals/hashCode), and weak so a
    // view that somehow skips onDropViewInstance can't pin state forever.
    private val viewStates = WeakHashMap<RlottieView, ViewState>()

    private fun stateFor(view: RlottieView): ViewState =
        viewStates.getOrPut(view) { ViewState() }

    // --- Identity -------------------------------------------------------------

    override fun getName(): String = "RlottieView"

    override fun createViewInstance(reactContext: ThemedReactContext): RlottieView {
        val view = RlottieView(reactContext)
        wireEvents(view)
        wireLifecycle(view, reactContext)
        return view
    }

    override fun onDropViewInstance(view: RlottieView) {
        // Detach listeners FIRST so no in-flight/queued work can dispatch an
        // event after this point, then release (which itself stops the
        // Choreographer as its first step, then tears down the native handle).
        view.onAnimationLoaded = null
        view.onAnimationFailure = null
        view.onAnimationStart = null
        view.onAnimationPause = null
        view.onAnimationLoop = null
        view.onAnimationFinish = null

        val state = viewStates.remove(view)
        val reactContext = view.context as? ThemedReactContext
        val listener = state?.lifecycleListener
        if (reactContext != null && listener != null) {
            reactContext.removeLifecycleEventListener(listener)
        }

        view.release()
        super.onDropViewInstance(view)
    }

    // --- Event wiring -----------------------------------------------------------

    private fun wireEvents(view: RlottieView) {
        view.onAnimationLoaded = { info -> emit(view, RlottieEvents.ON_ANIMATION_LOADED, RlottieEvents.loadedPayload(info)) }
        view.onAnimationFailure = { error -> emit(view, RlottieEvents.ON_ANIMATION_ERROR, RlottieEvents.errorPayload(error)) }
        view.onAnimationStart = { emit(view, RlottieEvents.ON_ANIMATION_START, RlottieEvents.emptyPayload()) }
        view.onAnimationPause = { emit(view, RlottieEvents.ON_ANIMATION_PAUSE, RlottieEvents.emptyPayload()) }
        view.onAnimationLoop = { emit(view, RlottieEvents.ON_ANIMATION_LOOP, RlottieEvents.emptyPayload()) }
        view.onAnimationFinish = { emit(view, RlottieEvents.ON_ANIMATION_FINISH, RlottieEvents.emptyPayload()) }
    }

    private fun emit(view: RlottieView, name: String, payload: com.facebook.react.bridge.WritableMap) {
        val reactContext = view.context as? ThemedReactContext ?: return
        val emitter = reactContext.getJSModule(RCTEventEmitter::class.java) ?: return
        RlottieEvents.emit(emitter, view.id, name, payload)
    }

    override fun getExportedCustomDirectEventTypeConstants(): MutableMap<String, Any> =
        RlottieEvents.exportedCustomDirectEventTypeConstants().toMutableMap()

    // --- pauseWhenInactive: gates a LifecycleEventListener --------------------

    private fun wireLifecycle(view: RlottieView, reactContext: ThemedReactContext) {
        val state = stateFor(view)
        val listener = object : LifecycleEventListener {
            override fun onHostResume() {
                if (state.pauseWhenInactive) view.onHostResume()
            }

            override fun onHostPause() {
                if (state.pauseWhenInactive) view.onHostPause()
            }

            override fun onHostDestroy() {
                if (state.pauseWhenInactive) view.onHostPause()
            }
        }
        state.lifecycleListener = listener
        reactContext.addLifecycleEventListener(listener)
    }

    // --- Props: source ----------------------------------------------------------
    //
    // Only the already-resolved shapes from the contract doc are handled here.
    // Everything else (uri / http(s) URL / require() asset id) is out of scope
    // for 3.3 — SEAM for Chunk 3.4's RlottieSourceResolver — and gets
    // `onAnimationError` with INVALID_SOURCE instead of silent failure or an
    // attempted resolution.

    @ReactProp(name = "source")
    fun setSource(view: RlottieView, source: ReadableMap?) {
        if (source == null) return

        val json = stringOrNull(source, "json")
        if (json != null) {
            val cacheKey = stringOrNull(source, "cacheKey") ?: ""
            val resourcePath = stringOrNull(source, "resourcePath")
            view.setSourceData(json, cacheKey, resourcePath)
            return
        }

        val path = stringOrNull(source, "path")
        if (path != null) {
            view.setSourceFile(path)
            return
        }

        // TODO(Chunk 3.4): route through RlottieSourceResolver instead of
        // erroring once bundled/asset/content-URI resolution exists.
        view.onAnimationFailure?.invoke(
            RlottiePlayerError(
                code = "INVALID_SOURCE",
                message = "Unsupported `source` shape; expected {json} or {path}. " +
                    "URI/asset resolution is not implemented in this build.",
            ),
        )
    }

    private fun stringOrNull(map: ReadableMap, key: String): String? {
        if (!map.hasKey(key) || map.isNull(key)) return null
        return map.getString(key)
    }

    // --- Props: playback-shaping (coalesced; see class doc) ---------------------

    @ReactProp(name = "loop", defaultBoolean = false)
    fun setLoop(view: RlottieView, value: Boolean) {
        val cfg = stateFor(view).config
        cfg.loop = value
        cfg.dirty = true
    }

    @ReactProp(name = "repeatCount", defaultInt = 0)
    fun setRepeatCount(view: RlottieView, value: Int) {
        val cfg = stateFor(view).config
        cfg.repeatCount = value
        cfg.dirty = true
    }

    @ReactProp(name = "speed", defaultDouble = 1.0)
    fun setSpeed(view: RlottieView, value: Double) {
        val cfg = stateFor(view).config
        cfg.speed = value
        cfg.dirty = true
    }

    @ReactProp(name = "startFrame", defaultInt = 0)
    fun setStartFrame(view: RlottieView, value: Int) {
        val cfg = stateFor(view).config
        cfg.startFrame = value
        cfg.dirty = true
    }

    @ReactProp(name = "endFrame", defaultInt = 0)
    fun setEndFrame(view: RlottieView, value: Int) {
        val cfg = stateFor(view).config
        cfg.endFrame = value
        cfg.dirty = true
    }

    @ReactProp(name = "autoPlay", defaultBoolean = false)
    fun setAutoPlay(view: RlottieView, value: Boolean) {
        val cfg = stateFor(view).config
        cfg.autoPlay = value
        cfg.dirty = true
    }

    override fun onAfterUpdateTransaction(view: RlottieView) {
        super.onAfterUpdateTransaction(view)
        val cfg = stateFor(view).config
        if (!cfg.dirty) return
        cfg.dirty = false
        view.configure(
            loop = cfg.loop,
            repeatCount = cfg.repeatCount,
            speed = cfg.speed,
            startFrame = cfg.startFrame,
            endFrame = cfg.endFrame,
            autoPlay = cfg.autoPlay,
        )
    }

    // --- Props: seek / progress (applied immediately, not coalesced — these are
    // one-shot imperative-style seeks, not part of the persisted playback config) ---

    @ReactProp(name = "progress", defaultDouble = 0.0)
    fun setProgress(view: RlottieView, value: Double) {
        view.seekToProgress(value)
    }

    // --- Props: rendering / misc -------------------------------------------------

    @ReactProp(name = "resizeMode")
    fun setResizeMode(view: RlottieView, value: String?) {
        // KNOWN GAP (flagged to the caller, not silently swallowed): RlottieView
        // (Chunk 3.2) always draws the current bitmap stretched to the exact
        // view bounds — it has no aspect-fit/fill/center hook, and the native
        // surface is always sized to the view's own aspect ratio, so there is
        // currently no seam to apply `contain`/`cover`/`stretch`/`center`
        // against. Accepted (so consumers don't crash passing it) and
        // validated, but has no visible effect yet. See the compile/verify
        // report for detail; needs a Chunk 3.2-or-later follow-up before this
        // prop is real.
        // Malformed values are ignored rather than thrown: an untrusted/typo'd
        // JS prop must never crash the whole app from a UI-thread prop apply.
        val mode = value ?: DEFAULT_RESIZE_MODE
        if (mode !in VALID_RESIZE_MODES) return
    }

    @ReactProp(name = "renderScale", defaultDouble = 1.0)
    fun setRenderScale(view: RlottieView, value: Double) {
        view.renderScale = value.toFloat()
    }

    @ReactProp(name = "pauseWhenInactive", defaultBoolean = true)
    fun setPauseWhenInactive(view: RlottieView, value: Boolean) {
        stateFor(view).pauseWhenInactive = value
    }

    @ReactProp(name = "cacheStrategy")
    fun setCacheStrategy(view: RlottieView, value: String?) {
        // KNOWN GAP, same shape as `resizeMode` above: neither RlottieBridge's
        // JNI surface (Chunk 3.1) nor RlottieView (Chunk 3.2) takes a cache
        // strategy anywhere — `setSourceData`/`setSourceFile` always pass
        // whatever cacheKey the `source` prop supplied. This prop is
        // realistically a Chunk 3.4 `RlottieSourceResolver`/`ModelCacheController`
        // concern (deciding whether to synthesize a cacheKey at all). Accepted
        // and validated here so the prop doesn't crash; not yet wired to
        // anything.
        val strategy = value ?: DEFAULT_CACHE_STRATEGY
        if (strategy !in VALID_CACHE_STRATEGIES) return
    }

    @ReactProp(name = "colorOverrides")
    fun setColorOverrides(view: RlottieView, value: ReadableArray?) {
        if (value == null) return
        for (i in 0 until value.size()) {
            val entry = value.getMap(i) ?: continue
            val keyPath = stringOrNull(entry, "keyPath") ?: continue
            val colorStr = stringOrNull(entry, "color") ?: continue
            val rgb = parseArgbOrRgb(colorStr) ?: continue
            view.setColor(keyPath, rgb[0], rgb[1], rgb[2])
        }
    }

    /**
     * Parses `#RRGGBB` or `#AARRGGBB` into `[r, g, b]` floats in `0..1`. Alpha
     * (if present) is dropped — `RlottieBridge.nativeSetColor` (Chunk 3.1) only
     * takes r/g/b.
     */
    private fun parseArgbOrRgb(color: String): FloatArray? {
        val hex = color.removePrefix("#")
        val rgbHex = when (hex.length) {
            6 -> hex
            8 -> hex.substring(2) // drop AA
            else -> return null
        }
        val value = rgbHex.toLongOrNull(16) ?: return null
        val r = ((value shr 16) and 0xFF).toFloat() / 255f
        val g = ((value shr 8) and 0xFF).toFloat() / 255f
        val b = (value and 0xFF).toFloat() / 255f
        return floatArrayOf(r, g, b)
    }

    // --- Commands -----------------------------------------------------------------
    //
    // RN's `dispatchViewManagerCommand` has shipped both int- and string-keyed
    // command ids across versions (plan §10); both overloads are implemented so
    // this library works regardless of which the host RN version uses.
    // `getCommandsMap()` supplies the name->id table the int-keyed path (and
    // some string-keyed paths that resolve through it) relies on.

    override fun getCommandsMap(): MutableMap<String, Int> {
        val builder = MapBuilder.builder<String, Int>()
        builder.put("play", COMMAND_PLAY)
        builder.put("pause", COMMAND_PAUSE)
        builder.put("resume", COMMAND_RESUME)
        builder.put("stop", COMMAND_STOP)
        builder.put("reset", COMMAND_RESET)
        builder.put("seekToProgress", COMMAND_SEEK_TO_PROGRESS)
        builder.put("seekToFrame", COMMAND_SEEK_TO_FRAME)
        builder.put("setSpeed", COMMAND_SET_SPEED)
        return builder.build()
    }

    override fun receiveCommand(root: RlottieView, commandId: Int, args: ReadableArray?) {
        executeCommand(root, commandId, args)
    }

    override fun receiveCommand(root: RlottieView, commandId: String, args: ReadableArray?) {
        // Some RN versions pass the stringified numeric id here rather than
        // the name registered in getCommandsMap(); accept either.
        val id = commandId.toIntOrNull() ?: COMMANDS_BY_NAME[commandId]
        if (id == null) return
        executeCommand(root, id, args)
    }

    private fun executeCommand(view: RlottieView, commandId: Int, args: ReadableArray?) {
        // Commands that arrive before the view has a live native handle are
        // dropped silently (contract doc) — every RlottieView method below is
        // already a no-op guarded by `handle == 0L`, so nothing extra is
        // needed here for that case. Malformed/missing args are treated the
        // same way: dropped, not an error.
        when (commandId) {
            COMMAND_PLAY -> executePlay(view, args)
            COMMAND_PAUSE -> view.pause()
            COMMAND_RESUME -> view.resume()
            COMMAND_STOP -> view.stop()
            COMMAND_RESET -> view.reset()
            COMMAND_SEEK_TO_PROGRESS -> {
                val progress = args?.let { if (it.size() > 0) it.getDouble(0) else null } ?: return
                view.seekToProgress(progress)
            }
            COMMAND_SEEK_TO_FRAME -> {
                val frame = args?.let { if (it.size() > 0) it.getInt(0) else null } ?: return
                view.seekToFrame(frame)
            }
            COMMAND_SET_SPEED -> {
                val speed = args?.let { if (it.size() > 0) it.getDouble(0) else null } ?: return
                view.setSpeed(speed)
            }
        }
    }

    /**
     * `play` takes `[startFrame: int | -1, endFrame: int | -1]`, where `-1`
     * means "unset". The override applies to THIS play only and is passed
     * straight through to `PlaybackController::play(optional, optional)` via
     * `nativePlay` — it must NOT be written into the persistent [PlaybackConfig],
     * or a later argument-less `play()` would silently inherit the one-off
     * range. iOS routes the identical values through `-playFromFrame:toFrame:`,
     * so both platforms land on the same core call.
     */
    private fun executePlay(view: RlottieView, args: ReadableArray?) {
        val start = args?.let { if (it.size() > 0) it.getInt(0) else NO_OVERRIDE } ?: NO_OVERRIDE
        val end = args?.let { if (it.size() > 1) it.getInt(1) else NO_OVERRIDE } ?: NO_OVERRIDE
        view.play(start, end)
    }

    private companion object {
        const val COMMAND_PLAY = 1
        const val COMMAND_PAUSE = 2
        const val COMMAND_RESUME = 3
        const val COMMAND_STOP = 4
        const val COMMAND_RESET = 5
        const val COMMAND_SEEK_TO_PROGRESS = 6
        const val COMMAND_SEEK_TO_FRAME = 7
        const val COMMAND_SET_SPEED = 8

        const val NO_OVERRIDE = -1

        val COMMANDS_BY_NAME = mapOf(
            "play" to COMMAND_PLAY,
            "pause" to COMMAND_PAUSE,
            "resume" to COMMAND_RESUME,
            "stop" to COMMAND_STOP,
            "reset" to COMMAND_RESET,
            "seekToProgress" to COMMAND_SEEK_TO_PROGRESS,
            "seekToFrame" to COMMAND_SEEK_TO_FRAME,
            "setSpeed" to COMMAND_SET_SPEED,
        )

        const val DEFAULT_RESIZE_MODE = "contain"
        val VALID_RESIZE_MODES = setOf("contain", "cover", "stretch", "center")

        const val DEFAULT_CACHE_STRATEGY = "model"
        val VALID_CACHE_STRATEGIES = setOf("none", "model")
    }
}
