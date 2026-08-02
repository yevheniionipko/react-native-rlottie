package com.rlottie

import com.facebook.react.ReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.uimanager.ViewManager

/**
 * Chunk 3.3 — registers [RlottieViewManager] with RN's autolinking machinery.
 *
 * `createNativeModules` is empty for now: the global config / source-resolution
 * module (`RlottieModule`) is Chunk 3.4 and will be added here then.
 */
class RlottiePackage : ReactPackage {

    override fun createViewManagers(reactContext: ReactApplicationContext): MutableList<ViewManager<*, *>> =
        mutableListOf(RlottieViewManager())

    override fun createNativeModules(reactContext: ReactApplicationContext): MutableList<NativeModule> =
        mutableListOf()
}
