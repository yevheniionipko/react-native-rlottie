package com.rlottie

import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.WritableMap
import com.facebook.react.common.MapBuilder
import com.facebook.react.uimanager.UIManagerHelper
import com.facebook.react.uimanager.events.EventDispatcher

/**
 * Chunk 3.3 — event-name constants and the `RlottieLoadedInfo`/`RlottiePlayerError` ->
 * `WritableMap` conversion, kept out of [RlottieView] on purpose: that view (Chunk 3.2)
 * knows nothing about RN types, only plain Kotlin data classes. This is the one place
 * that bridges the two.
 *
 * Payload shapes below are copied verbatim from `docs/bridge-contract.md`'s "Events —
 * direct events" table; keep them byte-for-byte in sync with that document (and with
 * iOS's `RNRlottieViewManager.mm`, which the contract also binds).
 *
 * Dispatch goes through [RlottieEvent] / [EventDispatcher] on both architectures
 * (docs/new-architecture-design.md §3.4.3). `ON_*` stay the bare `onX` names — they are
 * the registered `registrationName`; only the dispatched name is `top`-prefixed.
 */
object RlottieEvents {

    const val ON_ANIMATION_LOADED = "onAnimationLoaded"
    const val ON_ANIMATION_ERROR = "onAnimationError"
    const val ON_ANIMATION_START = "onAnimationStart"
    const val ON_ANIMATION_PAUSE = "onAnimationPause"
    const val ON_ANIMATION_LOOP = "onAnimationLoop"
    const val ON_ANIMATION_FINISH = "onAnimationFinish"
    const val ON_METRICS = "onMetrics"

    /**
     * All events in this component are direct (non-bubbling); see plan §11 / the contract
     * doc. Registers both the `onX` and `topX` keys so one dispatched name resolves on
     * both architectures; see docs/new-architecture-design.md §3.4.3.
     */
    fun exportedCustomDirectEventTypeConstants(): Map<String, Any> {
        val builder = MapBuilder.builder<String, Any>()
        for (name in ALL_EVENT_NAMES) {
            builder.put(name, MapBuilder.of("registrationName", name))
            builder.put(topName(name), MapBuilder.of("registrationName", name))
        }
        return builder.build()
    }

    /** `onX` -> `topX`, RN's historical convention for the dispatched event name. */
    fun topName(name: String): String = "top" + name.removePrefix("on")

    /** `{width, height, duration, frameRate, totalFrames, markers: [{name, startFrame, endFrame}]}` */
    fun loadedPayload(info: RlottieLoadedInfo): WritableMap {
        val map = Arguments.createMap()
        map.putInt("width", info.width)
        map.putInt("height", info.height)
        map.putDouble("duration", info.duration)
        map.putDouble("frameRate", info.frameRate)
        map.putInt("totalFrames", info.totalFrames)
        val markers = Arguments.createArray()
        for (marker in info.markers) {
            val markerMap = Arguments.createMap()
            markerMap.putString("name", marker.name)
            markerMap.putInt("startFrame", marker.startFrame)
            markerMap.putInt("endFrame", marker.endFrame)
            markers.pushMap(markerMap)
        }
        map.putArray("markers", markers)
        return map
    }

    /**
     * `{code, message}` — `code` is passed through verbatim from `cpp/ErrorCode.h` via the
     * native layer; this function must NOT re-map, re-case, or filter it (contract doc).
     */
    fun errorPayload(error: RlottiePlayerError): WritableMap {
        val map = Arguments.createMap()
        map.putString("code", error.code)
        map.putString("message", error.message)
        return map
    }

    /** `{}` — used for the four no-payload playback lifecycle events. */
    fun emptyPayload(): WritableMap = Arguments.createMap()

    /**
     * Byte-for-byte with `docs/bridge-contract.md`'s "Phase 7 additions" field table
     * (same keys/types as iOS's `onMetrics` block). The five counters are `putDouble`;
     * see that table's amendment note.
     */
    fun metricsPayload(info: RlottieMetricsInfo): WritableMap {
        val map = Arguments.createMap()
        map.putDouble("parseMs", info.parseMs)
        map.putDouble("firstFrameMs", info.firstFrameMs)
        map.putDouble("renderP50Ms", info.renderP50Ms)
        map.putDouble("renderP95Ms", info.renderP95Ms)
        map.putDouble("renderP99Ms", info.renderP99Ms)
        map.putDouble("framesRendered", info.framesRendered)
        map.putDouble("framesDropped", info.framesDropped)
        map.putDouble("bufferAllocCount", info.bufferAllocCount)
        map.putDouble("peakBufferBytes", info.peakBufferBytes)
        map.putDouble("uiStallCount", info.uiStallCount)
        map.putDouble("uiStallMaxMs", info.uiStallMaxMs)
        return map
    }

    /**
     * Emits a direct event for [viewId] through [EventDispatcher], which works on both
     * architectures (docs/new-architecture-design.md §3.4.3). Callers pass the bare `onX`
     * constant; this is the single place that applies the `top` prefix.
     */
    fun emit(dispatcher: EventDispatcher, surfaceId: Int, viewId: Int, name: String, payload: WritableMap) {
        dispatcher.dispatchEvent(RlottieEvent(surfaceId, viewId, topName(name), payload))
    }

    private val ALL_EVENT_NAMES = listOf(
        ON_ANIMATION_LOADED,
        ON_ANIMATION_ERROR,
        ON_ANIMATION_START,
        ON_ANIMATION_PAUSE,
        ON_ANIMATION_LOOP,
        ON_ANIMATION_FINISH,
        ON_METRICS,
    )
}
