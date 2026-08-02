package com.rlottie

import com.facebook.react.ReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.uimanager.ViewManager

/**
 * Chunk 3.3/3.4 — registers [RlottieViewManager] and [RlottieModule] with RN's
 * autolinking machinery. [RlottieModule] (Chunk 3.4) is the process-global
 * config module (`configure`/`clearModelCache`/`getNativeVersion`); it never
 * routes per-view playback, which stays exclusively on [RlottieViewManager].
 */
class RlottiePackage : ReactPackage {

    override fun createViewManagers(reactContext: ReactApplicationContext): MutableList<ViewManager<*, *>> =
        mutableListOf(RlottieViewManager())

    override fun createNativeModules(reactContext: ReactApplicationContext): MutableList<NativeModule> =
        mutableListOf(RlottieModule(reactContext))
}
