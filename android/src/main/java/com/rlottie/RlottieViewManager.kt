package com.rlottie

import com.facebook.react.bridge.LifecycleEventListener
import com.facebook.react.bridge.ReadableArray
import com.facebook.react.bridge.ReadableMap
import com.facebook.react.uimanager.SimpleViewManager
import com.facebook.react.uimanager.ThemedReactContext
import com.facebook.react.uimanager.UIManagerHelper
import com.facebook.react.uimanager.ViewManagerDelegate
import com.facebook.react.uimanager.annotations.ReactProp
import com.facebook.react.viewmanagers.RlottieViewManagerDelegate
import com.facebook.react.viewmanagers.RlottieViewManagerInterface
import java.util.WeakHashMap

/**
 * Chunk 3.3 — the `SimpleViewManager` half of the frozen bridge contract
 * (`docs/bridge-contract.md`), which this file must match byte-for-byte with
 * `ios/RNRlottieViewManager.mm`: prop names/defaults, the nine numeric
 * command ids, and the six direct-event names/payload shapes.
 *
 * Chunk 9.5 adds the Fabric side: this class also implements the codegen'd
 * `RlottieViewManagerInterface` and hands `getDelegate()` a
 * `RlottieViewManagerDelegate`, so the SAME setter/command methods now serve
 * both architectures — see `docs/new-architecture-design.md` §3.3/§3.5 for
 * why no `src/newarch`/`src/oldarch` split is needed here. `@ReactProp`
 * annotations stay: Legacy's native view config is still reflected from them,
 * independent of which delegate actually invokes the setter.
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
 * config, the last-applied source/progress, `pauseWhenInactive`, the
 * registered `LifecycleEventListener`) cannot live in manager fields — it is
 * keyed by view identity in [viewStates], which is torn down per-view in
 * [onDropViewInstance].
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
 *
 * ── The three Fabric change-guards (§3.5) ──────────────────────────────
 *
 * Under Fabric every setter runs on every commit with the COMPLETE prop set,
 * not a diff. `setSource`, the playback-shaping setters, and `setProgress`
 * each compare against a value stored in [ViewState] and no-op when nothing
 * actually changed, so an unrelated style-only re-render can't re-resolve the
 * source, re-anchor the playback clock, or re-seek. These guards are also
 * correct and harmless under Legacy — RN only calls a setter there when the
 * value changed, so the comparison always says "changed" — which is why they
 * are unconditional rather than gated on architecture.
 */
class RlottieViewManager :
    SimpleViewManager<RlottieView>(),
    RlottieViewManagerInterface<RlottieView> {

    /** Per-view mutable state; entries are removed explicitly in [onDropViewInstance]. */
    private class ViewState {
        val config = PlaybackConfig()
        var pauseWhenInactive: Boolean = true
        var lifecycleListener: LifecycleEventListener? = null

        // §3.5 guard #2 (progress): last value actually applied via seekToProgress
        // from the `progress` prop. Starts at the prop's own documented default
        // (0.0) so an initial `progress={0}` is indistinguishable from "never
        // set" — see §3.2.4.
        var lastProgress: Double = 0.0

        // §3.5 guard #3 (source): last identity applied via setSource. Null on
        // a fresh view so the very first setSource call always resolves.
        var lastSource: SourceIdentity? = null
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

    /**
     * §3.2.1's comparison key for `source`: `cacheKey`/`path`/`uri`/`resourcePath`
     * only — NEVER `json` (multi-megabyte; `cacheKey` is content-addressed, so
     * equal `cacheKey` already implies equal `json`).
     */
    private data class SourceIdentity(
        val cacheKey: String,
        val path: String,
        val uri: String,
        val resourcePath: String,
    )

    // Identity-keyed (RlottieView has no custom equals/hashCode), and weak so a
    // view that somehow skips onDropViewInstance can't pin state forever.
    private val viewStates = WeakHashMap<RlottieView, ViewState>()

    private fun stateFor(view: RlottieView): ViewState =
        viewStates.getOrPut(view) { ViewState() }

    // --- Fabric delegate --------------------------------------------------------

    private val delegate = RlottieViewManagerDelegate<RlottieView, RlottieViewManager>(this)

    override fun getDelegate(): ViewManagerDelegate<RlottieView> = delegate

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
        view.onMetrics = null

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
        view.onMetrics = { info -> emit(view, RlottieEvents.ON_METRICS, RlottieEvents.metricsPayload(info)) }
    }

    private fun emit(view: RlottieView, name: String, payload: com.facebook.react.bridge.WritableMap) {
        val reactContext = view.context as? ThemedReactContext ?: return
        val dispatcher = UIManagerHelper.getEventDispatcherForReactTag(reactContext, view.id) ?: return
        val surfaceId = UIManagerHelper.getSurfaceId(view)
        RlottieEvents.emit(dispatcher, surfaceId, view.id, name, payload)
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
    // All resolution (bundled asset / file / content URI / raw JSON, plus the
    // security checks in plan §16) happens in RlottieSourceResolver (Chunk
    // 3.4) so the C++ core never sees a URI. This setter only dispatches on
    // the resolver's result.
    //
    // §3.5 guard #3: compared BEFORE resolving, so an unchanged source never
    // reaches the resolver (no re-read, no content:// re-copy, no generation
    // bump / restart). Per §3.2.1: an empty cacheKey on either side is treated
    // as "changed" (conservative — matches today's "apply whatever arrives"
    // behaviour for a hand-rolled caller with no cache key).

    @ReactProp(name = "source")
    override fun setSource(view: RlottieView, value: ReadableMap?) {
        if (value == null) return

        val state = stateFor(view)
        val identity = sourceIdentity(value)
        val last = state.lastSource
        val changed = last == null || identity.cacheKey.isEmpty() || last.cacheKey.isEmpty() || identity != last
        if (!changed) return
        state.lastSource = identity

        when (val resolved = RlottieSourceResolver.resolve(view.context, value)) {
            is RlottieResolvedSource.Data ->
                view.setSourceData(resolved.json, resolved.cacheKey, resolved.resourcePath)

            is RlottieResolvedSource.File -> view.setSourceFile(resolved.path)

            is RlottieResolvedSource.Error ->
                view.onAnimationFailure?.invoke(
                    RlottiePlayerError(code = resolved.code, message = resolved.message),
                )
        }
    }

    private fun sourceIdentity(source: ReadableMap): SourceIdentity = SourceIdentity(
        cacheKey = stringOrNull(source, "cacheKey") ?: "",
        path = stringOrNull(source, "path") ?: "",
        uri = stringOrNull(source, "uri") ?: "",
        resourcePath = stringOrNull(source, "resourcePath") ?: "",
    )

    private fun stringOrNull(map: ReadableMap, key: String): String? {
        if (!map.hasKey(key) || map.isNull(key)) return null
        return map.getString(key)
    }

    // --- Props: playback-shaping (coalesced; see class doc) ---------------------
    //
    // §3.5 guard #1: each setter only marks [PlaybackConfig] dirty when the
    // incoming value actually differs from the stored one, so a Fabric commit
    // that re-sends an unchanged value never re-anchors the playback clock via
    // [onAfterUpdateTransaction].

    @ReactProp(name = "loop", defaultBoolean = false)
    override fun setLoop(view: RlottieView, value: Boolean) {
        val cfg = stateFor(view).config
        if (cfg.loop == value) return
        cfg.loop = value
        cfg.dirty = true
    }

    @ReactProp(name = "repeatCount", defaultInt = 0)
    override fun setRepeatCount(view: RlottieView, value: Int) {
        val cfg = stateFor(view).config
        if (cfg.repeatCount == value) return
        cfg.repeatCount = value
        cfg.dirty = true
    }

    @ReactProp(name = "speed", defaultDouble = 1.0)
    override fun setSpeed(view: RlottieView, value: Double) {
        val cfg = stateFor(view).config
        if (cfg.speed == value) return
        cfg.speed = value
        cfg.dirty = true
    }

    @ReactProp(name = "startFrame", defaultInt = 0)
    override fun setStartFrame(view: RlottieView, value: Int) {
        val cfg = stateFor(view).config
        if (cfg.startFrame == value) return
        cfg.startFrame = value
        cfg.dirty = true
    }

    @ReactProp(name = "endFrame", defaultInt = 0)
    override fun setEndFrame(view: RlottieView, value: Int) {
        val cfg = stateFor(view).config
        if (cfg.endFrame == value) return
        cfg.endFrame = value
        cfg.dirty = true
    }

    @ReactProp(name = "autoPlay", defaultBoolean = false)
    override fun setAutoPlay(view: RlottieView, value: Boolean) {
        val cfg = stateFor(view).config
        if (cfg.autoPlay == value) return
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
    //
    // §3.5 guard #2: seeks only when `progress` actually changed from the last
    // applied value, so a Fabric commit resending the same progress never
    // fights in-flight playback.

    @ReactProp(name = "progress", defaultDouble = 0.0)
    override fun setProgress(view: RlottieView, value: Double) {
        val state = stateFor(view)
        if (state.lastProgress == value) return
        state.lastProgress = value
        view.seekToProgress(value)
    }

    // --- Props: rendering / misc -------------------------------------------------

    @ReactProp(name = "resizeMode")
    override fun setResizeMode(view: RlottieView, value: String?) {
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
    override fun setRenderScale(view: RlottieView, value: Double) {
        view.renderScale = value.toFloat()
    }

    @ReactProp(name = "pauseWhenInactive", defaultBoolean = true)
    override fun setPauseWhenInactive(view: RlottieView, value: Boolean) {
        stateFor(view).pauseWhenInactive = value
    }

    // Phase 7 addition (docs/bridge-contract.md "Phase 7 additions"). Applied
    // immediately, same as `pauseWhenInactive` above — gating collection
    // on/off has no ordering dependency on anything else in a prop
    // transaction.
    @ReactProp(name = "metricsEnabled", defaultBoolean = false)
    override fun setMetricsEnabled(view: RlottieView, value: Boolean) {
        view.metricsEnabled = value
    }

    @ReactProp(name = "cacheStrategy")
    override fun setCacheStrategy(view: RlottieView, value: String?) {
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
    override fun setColorOverrides(view: RlottieView, value: ReadableArray?) {
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

    // --- Props: Phase 6 dynamic properties (docs/bridge-contract.md's "Phase 6
    // additions") ----------------------------------------------------------------
    //
    // Parallel in shape to `colorOverrides` above, deliberately not merged into
    // one polymorphic array (see the contract). Applied immediately, same as
    // `colorOverrides` — each entry is an independent, idempotent native call.
    // A malformed entry (missing key, wrong type, non-finite number) is
    // skipped rather than crashing the whole prop apply; out-of-range-but-
    // finite values (e.g. opacity 5.0, width -3.0) are passed through and
    // clamped natively (RenderCoordinator::setOpacity/setStrokeWidth), not
    // rejected here — mirrors iOS's RNRlottieFiniteNumber/RCT_CUSTOM_VIEW_PROPERTY
    // handling exactly.

    @ReactProp(name = "opacityOverrides")
    override fun setOpacityOverrides(view: RlottieView, value: ReadableArray?) {
        if (value == null) return
        for (i in 0 until value.size()) {
            val entry = value.getMap(i) ?: continue
            val keyPath = stringOrNull(entry, "keyPath") ?: continue
            val opacity = finiteDoubleOrNull(entry, "opacity") ?: continue
            view.setOpacity(keyPath, opacity.toFloat())
        }
    }

    @ReactProp(name = "strokeWidthOverrides")
    override fun setStrokeWidthOverrides(view: RlottieView, value: ReadableArray?) {
        if (value == null) return
        for (i in 0 until value.size()) {
            val entry = value.getMap(i) ?: continue
            val keyPath = stringOrNull(entry, "keyPath") ?: continue
            val width = finiteDoubleOrNull(entry, "width") ?: continue
            view.setStrokeWidth(keyPath, width.toFloat())
        }
    }

    /**
     * Reads [key] as a finite `Double`, or `null` for a missing key, a
     * non-numeric type, or a non-finite value (NaN/Infinity) — the same
     * defensive checks iOS's `RNRlottieFiniteNumber` applies, so a malformed
     * override entry never crashes a render on either platform.
     */
    private fun finiteDoubleOrNull(map: ReadableMap, key: String): Double? {
        if (!map.hasKey(key) || map.isNull(key)) return null
        if (map.getType(key) != com.facebook.react.bridge.ReadableType.Number) return null
        val v = map.getDouble(key)
        return if (v.isFinite()) v else null
    }

    // --- Commands: typed bodies (RlottieViewManagerInterface) --------------------
    //
    // These are the real, single bodies both `receiveCommand` overloads below
    // route to — the Fabric delegate's generated `receiveCommand(view, String,
    // args)` also calls these directly (see RlottieViewManagerDelegate.java).

    override fun play(view: RlottieView, startFrame: Int, endFrame: Int) {
        // startFrame/endFrame apply to THIS play only (-1 = unset) and are
        // passed straight through to PlaybackController::play(optional,
        // optional) — never written into the persistent PlaybackConfig, or a
        // later argument-less play() would silently inherit the one-off range.
        view.play(startFrame, endFrame)
    }

    override fun pause(view: RlottieView) {
        view.pause()
    }

    override fun resume(view: RlottieView) {
        view.resume()
    }

    override fun stop(view: RlottieView) {
        view.stop()
    }

    override fun reset(view: RlottieView) {
        view.reset()
    }

    override fun seekToProgress(view: RlottieView, progress: Double) {
        view.seekToProgress(progress)
    }

    override fun seekToFrame(view: RlottieView, frame: Int) {
        view.seekToFrame(frame)
    }

    override fun setPlaybackSpeed(view: RlottieView, speed: Double) {
        view.setSpeed(speed)
    }

    override fun playMarker(view: RlottieView, name: String) {
        // Return value intentionally ignored — an unknown marker is a silent
        // no-op per docs/bridge-contract.md, not an error/event.
        view.playMarker(name)
    }

    // --- Commands: Legacy numeric-id dispatch -------------------------------------
    //
    // RN's `dispatchViewManagerCommand` has shipped both int- and string-keyed
    // command ids across versions (plan §10); both overloads are implemented so
    // this library works regardless of which the host RN version uses.
    // `getCommandsMap()` supplies the name->id table the int-keyed path (and
    // some string-keyed paths that resolve through it) relies on. These ids are
    // Legacy-only (docs/new-architecture-design.md §3.3): Fabric dispatches
    // commands by name only, through the typed methods above via the generated
    // delegate.

    // Deliberately NOT built with `com.facebook.react.common.MapBuilder`, which
    // is the conventional helper here and was used originally. As of RN 0.81
    // MapBuilder is itself Kotlin and its `build()` returns a READ-ONLY
    // `kotlin.collections.Map<K, V>`, which cannot satisfy this override's
    // `MutableMap<String, Int>` return type — a genuine compile error
    // ("type mismatch"), not a style preference. It was invisible until Chunk
    // 8.1 built the example app, because nothing before that compiled this
    // module's Kotlin against a real React Native artifact. Copying into a
    // HashMap would also work; building the mutable map directly avoids
    // allocating two maps to return one.
    override fun getCommandsMap(): MutableMap<String, Int> =
        mutableMapOf(
            "play" to COMMAND_PLAY,
            "pause" to COMMAND_PAUSE,
            "resume" to COMMAND_RESUME,
            "stop" to COMMAND_STOP,
            "reset" to COMMAND_RESET,
            "seekToProgress" to COMMAND_SEEK_TO_PROGRESS,
            "seekToFrame" to COMMAND_SEEK_TO_FRAME,
            "setSpeed" to COMMAND_SET_SPEED,
            "playMarker" to COMMAND_PLAY_MARKER,
        )

    override fun receiveCommand(root: RlottieView, commandId: Int, args: ReadableArray?) {
        // Commands that arrive before the view has a live native handle are
        // dropped silently (contract doc) — every RlottieView method below is
        // already a no-op guarded by `handle == 0L`, so nothing extra is
        // needed here for that case. Malformed/missing args are treated the
        // same way: dropped, not an error.
        when (commandId) {
            COMMAND_PLAY -> {
                val start = args?.let { if (it.size() > 0) it.getInt(0) else NO_OVERRIDE } ?: NO_OVERRIDE
                val end = args?.let { if (it.size() > 1) it.getInt(1) else NO_OVERRIDE } ?: NO_OVERRIDE
                play(root, start, end)
            }
            COMMAND_PAUSE -> pause(root)
            COMMAND_RESUME -> resume(root)
            COMMAND_STOP -> stop(root)
            COMMAND_RESET -> reset(root)
            COMMAND_SEEK_TO_PROGRESS -> {
                val progress = args?.let { if (it.size() > 0) it.getDouble(0) else null } ?: return
                seekToProgress(root, progress)
            }
            COMMAND_SEEK_TO_FRAME -> {
                val frame = args?.let { if (it.size() > 0) it.getInt(0) else null } ?: return
                seekToFrame(root, frame)
            }
            COMMAND_SET_SPEED -> {
                val speed = args?.let { if (it.size() > 0) it.getDouble(0) else null } ?: return
                setPlaybackSpeed(root, speed)
            }
            COMMAND_PLAY_MARKER -> {
                val name = args?.let { if (it.size() > 0) it.getString(0) else null } ?: return
                playMarker(root, name)
            }
        }
    }

    override fun receiveCommand(root: RlottieView, commandId: String, args: ReadableArray?) {
        // Three accepted forms, in order:
        //  1. a stringified numeric id (some RN versions send this);
        //  2. a frozen Legacy command NAME from getCommandsMap() — notably
        //     "setSpeed", which the generated delegate does NOT know because
        //     the Fabric spec had to rename it to "setPlaybackSpeed" (§3.3).
        //     Without this the JS name-dispatch fallback would silently drop
        //     that one command on the Legacy path;
        //  3. anything else -> the generated delegate, via super, which decodes
        //     the Fabric names into the typed methods above. Not bypassed, so
        //     the decode is not duplicated here.
        val numericId = commandId.toIntOrNull() ?: getCommandsMap()[commandId]
        if (numericId != null) {
            receiveCommand(root, numericId, args)
            return
        }
        super.receiveCommand(root, commandId, args)
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
        const val COMMAND_PLAY_MARKER = 9

        const val NO_OVERRIDE = -1

        const val DEFAULT_RESIZE_MODE = "contain"
        val VALID_RESIZE_MODES = setOf("contain", "cover", "stretch", "center")

        const val DEFAULT_CACHE_STRATEGY = "model"
        val VALID_CACHE_STRATEGIES = setOf("none", "model")
    }
}
