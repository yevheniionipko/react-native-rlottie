package com.rlottie

import com.facebook.react.bridge.WritableMap
import com.facebook.react.uimanager.events.Event

/**
 * The one `Event` subclass all seven RlottieView events dispatch through, on both
 * architectures (docs/new-architecture-design.md §3.4.3).
 * `name` must already be the `top`-prefixed form; callers choose it.
 */
class RlottieEvent(
    surfaceId: Int,
    viewTag: Int,
    private val name: String,
    private val payload: WritableMap,
) : Event<RlottieEvent>(surfaceId, viewTag) {

    override fun getEventName(): String = name

    // MANDATORY: Event defaults to true with getCoalescingKey() = 0, which
    // would coalesce e.g. two onAnimationLoop events in one batch into one.
    override fun canCoalesce(): Boolean = false

    override fun getEventData(): WritableMap? = payload
}
