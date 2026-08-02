// Chunk 3.1 — the narrow JNI surface for react-native-rlottie.
//
// Kotlin binds these as static natives on `com.rlottie.RlottieBridge`
// (declared in Chunk 3.2). Every function that takes a handle validates it via
// asHandle() before use; an invalid/stale/zero handle fails safe.
//
// Threading contract (enforced by the Kotlin caller):
//   * nativeAdvance + all command/config/seek natives run on the UI thread
//     (they touch PlaybackController, which is [UI]-only).
//   * setSource/setSurfaceSize/setColor are thread-safe on the coordinator.
//   * nativeCopyFrontInto / nativePollEvent / nativeHasNewFrame run on the UI
//     thread (they present / drain the poll-based sink).
//   * nativeDestroy is called exactly once (UI thread) at teardown.
#include <android/bitmap.h>
#include <jni.h>

#include <cstdint>
#include <new>
#include <optional>
#include <string>
#include <utility>

#include "AndroidEventEncoding.h"
#include "AndroidPixelConvert.h"
#include "AnimationSource.h"
#include "JniPlayerHandle.h"
#include "ModelCacheController.h"
#include "PixelFormat.h"
#include "PlaybackController.h"
#include "PlayerError.h"
#include "RlottieVersion.h"

namespace {

using rnrlottie::JniPlayerHandle;

// Validate the opaque jlong; nullptr for 0 or a destroyed handle. The check
// itself lives in JniPlayerHandle.cpp so the fault-injection tests exercise it.
JniPlayerHandle* asHandle(jlong handle) {
    return rnrlottie::lookupPlayerHandle(static_cast<std::int64_t>(handle));
}

std::string toString(JNIEnv* env, jstring s) {
    if (s == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(s, nullptr);
    if (chars == nullptr) {
        return {};
    }
    std::string out(chars);
    env->ReleaseStringUTFChars(s, chars);
    return out;
}

std::size_t clampNonNegative(jint v) {
    return v > 0 ? static_cast<std::size_t>(v) : 0;
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) {
    // No JavaVM caching needed: the sink is poll-based and never calls into Java.
    return JNI_VERSION_1_6;
}

JNIEXPORT jlong JNICALL Java_com_rlottie_RlottieBridge_nativeCreate(JNIEnv*, jclass,
                                                                    jlong maxPixels) {
    const std::size_t mp = maxPixels > 0 ? static_cast<std::size_t>(maxPixels) : 0;
    return static_cast<jlong>(rnrlottie::createPlayerHandle(mp));  // 0 on OOM
}

JNIEXPORT void JNICALL Java_com_rlottie_RlottieBridge_nativeSetSourceData(
    JNIEnv* env, jclass, jlong handle, jstring json, jstring cacheKey,
    jstring resourcePath) {
    auto* h = asHandle(handle);
    if (h == nullptr) return;
    rnrlottie::AnimationSource src;
    src.kind = rnrlottie::AnimationSource::Kind::Data;
    src.json = toString(env, json);
    src.cacheKey = toString(env, cacheKey);
    src.resourcePath = toString(env, resourcePath);
    src.useModelCache = true;
    h->coordinator().setSource(std::move(src));
}

JNIEXPORT void JNICALL Java_com_rlottie_RlottieBridge_nativeSetSourceFile(
    JNIEnv* env, jclass, jlong handle, jstring path) {
    auto* h = asHandle(handle);
    if (h == nullptr) return;
    rnrlottie::AnimationSource src;
    src.kind = rnrlottie::AnimationSource::Kind::File;
    src.path = toString(env, path);
    src.useModelCache = true;
    h->coordinator().setSource(std::move(src));
}

JNIEXPORT void JNICALL Java_com_rlottie_RlottieBridge_nativeSetSurfaceSize(
    JNIEnv*, jclass, jlong handle, jint width, jint height) {
    auto* h = asHandle(handle);
    if (h == nullptr || width <= 0 || height <= 0) return;
    h->coordinator().setSurfaceSize(static_cast<std::size_t>(width),
                                    static_cast<std::size_t>(height));
}

JNIEXPORT void JNICALL Java_com_rlottie_RlottieBridge_nativeConfigure(
    JNIEnv*, jclass, jlong handle, jboolean loop, jint repeatCount, jdouble speed,
    jint startFrame, jint endFrame, jboolean autoPlay) {
    auto* h = asHandle(handle);
    if (h == nullptr) return;
    rnrlottie::PlaybackConfig cfg;
    cfg.loop = loop == JNI_TRUE;
    cfg.repeatCount = repeatCount;
    cfg.speed = speed;
    cfg.startFrame = clampNonNegative(startFrame);
    cfg.endFrame = clampNonNegative(endFrame);
    cfg.autoPlay = autoPlay == JNI_TRUE;
    h->playback().configure(cfg);
}

// `startFrame`/`endFrame` are a ONE-OFF segment override for this play only
// (plan §3's `play(options?)`); -1 means "unset", per docs/bridge-contract.md.
// They deliberately do NOT mutate the persistent PlaybackConfig — a later
// play() with no args must fall back to the configured range. This mirrors
// iOS's -playFromFrame:toFrame:, and both delegate the actual semantics to
// PlaybackController::play(optional, optional).
//
// The sentinel must be checked here: std::size_t cannot represent -1, and 0 is
// a legitimate frame number, so a `> 0` truthiness test would be wrong.
JNIEXPORT void JNICALL Java_com_rlottie_RlottieBridge_nativePlay(JNIEnv*, jclass,
                                                                jlong handle,
                                                                jint startFrame,
                                                                jint endFrame) {
    auto* h = asHandle(handle);
    if (h == nullptr) return;
    const std::optional<std::size_t> sf =
        startFrame >= 0 ? std::optional<std::size_t>(static_cast<std::size_t>(startFrame))
                        : std::nullopt;
    const std::optional<std::size_t> ef =
        endFrame >= 0 ? std::optional<std::size_t>(static_cast<std::size_t>(endFrame))
                      : std::nullopt;
    h->playback().play(sf, ef);
}

JNIEXPORT void JNICALL Java_com_rlottie_RlottieBridge_nativePause(JNIEnv*, jclass,
                                                                 jlong handle) {
    auto* h = asHandle(handle);
    if (h) h->playback().pause();
}

JNIEXPORT void JNICALL Java_com_rlottie_RlottieBridge_nativeResume(JNIEnv*, jclass,
                                                                  jlong handle) {
    auto* h = asHandle(handle);
    if (h) h->playback().resume();
}

JNIEXPORT void JNICALL Java_com_rlottie_RlottieBridge_nativeStop(JNIEnv*, jclass,
                                                                jlong handle) {
    auto* h = asHandle(handle);
    if (h) h->playback().stop();
}

JNIEXPORT void JNICALL Java_com_rlottie_RlottieBridge_nativeReset(JNIEnv*, jclass,
                                                                 jlong handle) {
    auto* h = asHandle(handle);
    if (h) h->playback().reset();
}

JNIEXPORT void JNICALL Java_com_rlottie_RlottieBridge_nativeSeekFrame(JNIEnv*, jclass,
                                                                     jlong handle,
                                                                     jint frame) {
    auto* h = asHandle(handle);
    if (h) h->playback().seekToFrame(clampNonNegative(frame));
}

JNIEXPORT void JNICALL Java_com_rlottie_RlottieBridge_nativeSeekProgress(
    JNIEnv*, jclass, jlong handle, jdouble progress) {
    auto* h = asHandle(handle);
    if (h) h->playback().seekToProgress(progress);
}

JNIEXPORT void JNICALL Java_com_rlottie_RlottieBridge_nativeSetSpeed(JNIEnv*, jclass,
                                                                    jlong handle,
                                                                    jdouble speed) {
    auto* h = asHandle(handle);
    if (h) h->playback().setSpeed(speed);
}

// Returns the PlaybackEvent ordinal so Kotlin can emit lifecycle events:
//   0=None, 1=Started, 2=Paused, 3=Loop, 4=Finished.
JNIEXPORT jint JNICALL Java_com_rlottie_RlottieBridge_nativeAdvance(JNIEnv*, jclass,
                                                                   jlong handle,
                                                                   jdouble monotonicSeconds) {
    auto* h = asHandle(handle);
    if (h == nullptr) return 0;
    const auto tick = h->playback().advance(monotonicSeconds);
    if (tick.shouldRender) {
        h->coordinator().requestFrame(tick.frame, h->coordinator().generation());
    }
    return static_cast<jint>(tick.event);
}

JNIEXPORT jboolean JNICALL Java_com_rlottie_RlottieBridge_nativeHasNewFrame(
    JNIEnv*, jclass, jlong handle) {
    auto* h = asHandle(handle);
    if (h == nullptr) return JNI_FALSE;
    return h->sinkState().newFrame.load(std::memory_order_acquire) ? JNI_TRUE : JNI_FALSE;
}

// Copies the front buffer into an ARGB_8888 Bitmap, swapping R<->B (rlottie is
// B,G,R,A; Android ARGB_8888 native memory is R,G,B,A). Premultiplication already
// matches. Returns false (and presents nothing) when there is no frame or the
// bitmap format/dimensions do not match the front buffer.
JNIEXPORT jboolean JNICALL Java_com_rlottie_RlottieBridge_nativeCopyFrontInto(
    JNIEnv* env, jclass, jlong handle, jobject bitmap) {
    auto* h = asHandle(handle);
    if (h == nullptr || bitmap == nullptr) return JNI_FALSE;

    const auto front = h->coordinator().frameBuffer().readFront();
    if (front.pixels == nullptr) return JNI_FALSE;

    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS) {
        return JNI_FALSE;
    }
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 ||
        info.width != front.width || info.height != front.height) {
        return JNI_FALSE;
    }

    void* dst = nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &dst) != ANDROID_BITMAP_RESULT_SUCCESS) {
        return JNI_FALSE;
    }

    rnrlottie::rlottieSurfaceToAndroid(front.pixels, front.bytesPerLine, dst, info.stride,
                                       front.width, front.height);

    AndroidBitmap_unlockPixels(env, bitmap);
    h->sinkState().newFrame.store(false, std::memory_order_release);
    return JNI_TRUE;
}

// Pops one queued load/error event as an encoded string (null when empty).
// Applies the corresponding PlaybackController transition on the UI thread so
// playback state stays in sync (onLoaded / onLoadFailed).
JNIEXPORT jstring JNICALL Java_com_rlottie_RlottieBridge_nativePollEvent(JNIEnv* env,
                                                                        jclass,
                                                                        jlong handle) {
    auto* h = asHandle(handle);
    if (h == nullptr) return nullptr;

    rnrlottie::PlayerEvent ev;
    if (!rnrlottie::popEvent(h->sinkState(), ev)) {
        return nullptr;
    }

    if (ev.type == rnrlottie::PlayerEvent::Type::Loaded) {
        h->playback().onLoaded(ev.metadata);
    } else if (h->playback().state() == rnrlottie::PlaybackState::Empty ||
               h->playback().state() == rnrlottie::PlaybackState::Loading) {
        // Treat an error before a successful load as a load failure.
        h->playback().onLoadFailed(ev.error);
    }

    const std::string encoded = rnrlottie::encodeEvent(ev);
    return env->NewStringUTF(encoded.c_str());
}

JNIEXPORT void JNICALL Java_com_rlottie_RlottieBridge_nativeSetColor(
    JNIEnv* env, jclass, jlong handle, jstring keyPath, jfloat r, jfloat g, jfloat b) {
    auto* h = asHandle(handle);
    if (h == nullptr) return;
    h->coordinator().setColor(toString(env, keyPath), r, g, b);
}

JNIEXPORT void JNICALL Java_com_rlottie_RlottieBridge_nativeDestroy(JNIEnv*, jclass,
                                                                   jlong handle) {
    // Idempotent; a zero/stale/already-destroyed handle is a no-op.
    rnrlottie::destroyPlayerHandle(static_cast<std::int64_t>(handle));
}

// --- Global / process-wide (Chunk 3.4) --------------------------------------
//
// Unlike everything above, these take no handle: ModelCacheController is
// process-global state (rlottie's own LOTTIE_CACHE), internally serialized by
// its own mutex (cpp/ModelCacheController.cpp), so no per-call handle
// validation applies. Backs the global `RlottieModule` (RlottieModule.kt),
// never per-view playback (plan §15 / docs/bridge-contract.md).

JNIEXPORT void JNICALL Java_com_rlottie_RlottieBridge_nativeSetModelCacheSize(
    JNIEnv*, jclass, jlong entries) {
    const std::size_t size = entries > 0 ? static_cast<std::size_t>(entries) : 0;
    rnrlottie::ModelCacheController::setModelCacheSize(size);
}

JNIEXPORT void JNICALL Java_com_rlottie_RlottieBridge_nativeClearModelCache(JNIEnv*,
                                                                           jclass) {
    rnrlottie::ModelCacheController::clearModelCache();
}

JNIEXPORT jlong JNICALL Java_com_rlottie_RlottieBridge_nativeGetModelCacheSize(JNIEnv*,
                                                                              jclass) {
    return static_cast<jlong>(rnrlottie::ModelCacheController::currentSize());
}

// kRlottieCommit is a compile-time literal (RlottieVersion.h); NewStringUTF
// can only fail via a Java-level OOM, which is a normal JNI exception state
// left for the JVM to handle — not a C++ exception crossing this boundary.
JNIEXPORT jstring JNICALL Java_com_rlottie_RlottieBridge_nativeGetRlottieCommit(
    JNIEnv* env, jclass) {
    return env->NewStringUTF(rnrlottie::kRlottieCommit);
}

}  // extern "C"
