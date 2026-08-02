package com.rlottie

import com.facebook.react.BaseReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.module.model.ReactModuleInfo
import com.facebook.react.module.model.ReactModuleInfoProvider
import com.facebook.react.uimanager.ViewManager
import com.rlottie.spec.NativeRlottieModuleSpec

/**
 * Chunk 3.3/3.4/9.3 — registers [RlottieViewManager] and [RlottieModule] with
 * RN's autolinking machinery. [RlottieModule] is the process-global config
 * module (`configure`/`clearModelCache`/`getNativeVersion`); it never routes
 * per-view playback, which stays exclusively on [RlottieViewManager].
 *
 * `BaseReactPackage` (rather than plain `ReactPackage` +
 * `createNativeModules`) is what lets [RlottieModule] work as a TurboModule
 * and as a legacy module from one class — see
 * docs/new-architecture-design.md §2.4.
 */
class RlottiePackage : BaseReactPackage() {

    override fun getModule(name: String, reactContext: ReactApplicationContext): NativeModule? =
        if (name == NativeRlottieModuleSpec.NAME) RlottieModule(reactContext) else null

    override fun createViewManagers(reactContext: ReactApplicationContext): MutableList<ViewManager<*, *>> =
        mutableListOf(RlottieViewManager())

    override fun getReactModuleInfoProvider(): ReactModuleInfoProvider = ReactModuleInfoProvider {
        mapOf(
            NativeRlottieModuleSpec.NAME to ReactModuleInfo(
                NativeRlottieModuleSpec.NAME,
                NativeRlottieModuleSpec.NAME,
                false, // canOverrideExistingModule
                false, // needsEagerInit
                false, // isCxxModule
                true, // isTurboModule
            ),
        )
    }
}
