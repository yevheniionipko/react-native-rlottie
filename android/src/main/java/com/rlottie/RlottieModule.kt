package com.rlottie

import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.bridge.ReadableMap
import com.facebook.react.bridge.WritableMap

/**
 * Chunk 3.4 — the Android half of the global config module
 * (docs/bridge-contract.md, "Global module"). Exposes **only** process-global
 * operations on rlottie's library-level model cache (`ModelCacheController`,
 * plan §15) via the new global JNI natives added to `RlottieBridge`/
 * `RlottieJni.cpp` alongside this file. Never routes per-view playback — that
 * stays entirely inside [RlottieViewManager]/[RlottieView].
 *
 * Method surface must match the contract table exactly:
 *  - `configure({modelCacheSize})` -> `ModelCacheController::setModelCacheSize`
 *  - `clearModelCache()` -> `ModelCacheController::clearModelCache`
 *  - `getNativeVersion()` -> `{rlottieCommit, modelCacheSize}`
 *
 * All three are Promise-based `@ReactMethod`s (the idiomatic legacy-bridge
 * shape for a value-returning/asynchronous native module call); nothing here
 * throws across the JNI boundary underneath, but a Kotlin-level failure is
 * still routed through `promise.reject` rather than propagated, so a bad call
 * from JS surfaces as a rejected promise, not a crash.
 */
class RlottieModule(reactContext: ReactApplicationContext) :
    ReactContextBaseJavaModule(reactContext) {

    override fun getName(): String = NAME

    @ReactMethod
    fun configure(options: ReadableMap?, promise: Promise) {
        try {
            val hasSize = options != null &&
                options.hasKey(KEY_MODEL_CACHE_SIZE) &&
                !options.isNull(KEY_MODEL_CACHE_SIZE)
            if (hasSize && options != null) {
                val size = options.getInt(KEY_MODEL_CACHE_SIZE)
                if (size < 0) {
                    promise.reject(
                        ERROR_INVALID_ARGUMENT,
                        "modelCacheSize must be >= 0",
                    )
                    return
                }
                RlottieBridge.nativeSetModelCacheSize(size.toLong())
            }
            promise.resolve(null)
        } catch (e: Exception) {
            promise.reject(ERROR_CONFIGURE_FAILED, e.message, e)
        }
    }

    @ReactMethod
    fun clearModelCache(promise: Promise) {
        try {
            RlottieBridge.nativeClearModelCache()
            promise.resolve(null)
        } catch (e: Exception) {
            promise.reject(ERROR_CLEAR_MODEL_CACHE_FAILED, e.message, e)
        }
    }

    @ReactMethod
    fun getNativeVersion(promise: Promise) {
        try {
            val result: WritableMap = Arguments.createMap()
            result.putString("rlottieCommit", RlottieBridge.nativeGetRlottieCommit())
            result.putInt("modelCacheSize", RlottieBridge.nativeGetModelCacheSize().toInt())
            promise.resolve(result)
        } catch (e: Exception) {
            promise.reject(ERROR_GET_NATIVE_VERSION_FAILED, e.message, e)
        }
    }

    private companion object {
        const val NAME = "RlottieModule"
        const val KEY_MODEL_CACHE_SIZE = "modelCacheSize"

        const val ERROR_INVALID_ARGUMENT = "INVALID_ARGUMENT"
        const val ERROR_CONFIGURE_FAILED = "CONFIGURE_FAILED"
        const val ERROR_CLEAR_MODEL_CACHE_FAILED = "CLEAR_MODEL_CACHE_FAILED"
        const val ERROR_GET_NATIVE_VERSION_FAILED = "GET_NATIVE_VERSION_FAILED"
    }
}
