# react-native-rlottie — Android native build.
# Builds the JNI shared library: the narrow JNI surface (Chunk 3.1) + the shared
# C++ core (Phase 1) linked against the vendored rlottie static lib (Chunk 0.2).
cmake_minimum_required(VERSION 3.18)
project(react_native_rlottie LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Repo root, relative to android/src/main/cpp.
get_filename_component(RN_RLOTTIE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)

# rlottie build options (plan §14). rlottie pins its own -std=c++14,
# -fno-exceptions, -fno-rtti PRIVATE on its target, so these do NOT affect our
# C++17 library. LOTTIE_EXAMPLE is the vendored-copy guard added in Chunk 0.2.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)   # static-link rlottie
set(LOTTIE_THREAD  ON  CACHE BOOL "" FORCE)
set(LOTTIE_CACHE   ON  CACHE BOOL "" FORCE)
set(LOTTIE_MODULE  OFF CACHE BOOL "" FORCE)       # no dynamic image-loader plugin
set(LOTTIE_TEST    OFF CACHE BOOL "" FORCE)
set(LOTTIE_EXAMPLE OFF CACHE BOOL "" FORCE)

add_subdirectory(
    ${RN_RLOTTIE_ROOT}/cpp/third_party/rlottie
    ${CMAKE_CURRENT_BINARY_DIR}/rlottie
)

# --- 32-bit ARM: force rlottie's generic (non-NEON) render path ---------------
#
# On armeabi-v7a the NDK compiler defines __ARM_NEON__, so rlottie compiles
# vdrawhelper_neon.cpp, which calls pixman_composite_*_asm_neon. Those live in
# src/vector/pixman/pixman-arm-neon-asm.S, which rlottie's pixman/CMakeLists only
# adds when the variable `ARCH` equals "arm" — and nothing in the NDK toolchain
# ever sets ARCH. Left alone, every source compiles and the LINK fails with
# `undefined symbol: pixman_composite_src_n_8888_asm_neon`.
#
# Setting ARCH="arm" to pull the .S in does NOT work either: it is GNU-as syntax
# that LLVM's integrated assembler rejects ("unknown directive"), and NDK r28 no
# longer ships GNU as, so -fno-integrated-as has nothing to fall back to.
#
# So we do what the podspec and the host tests already do on Apple arm64:
# undefine __ARM_NEON__ for rlottie's own sources, which compiles out
# vdrawhelper_neon.cpp entirely and uses the portable C path. armeabi-v7a is
# opt-in (see abiFilters in android/build.gradle) and loses NEON acceleration;
# arm64-v8a — the ABI that matters — is unaffected and keeps its own path.
if(ANDROID_ABI STREQUAL "armeabi-v7a")
    target_compile_options(rlottie PRIVATE -U__ARM_NEON__)
endif()

# --- Do NOT let rlottie's version script reach our link line ------------------
#
# rlottie attaches its own linker version script as a PRIVATE link item:
#   target_link_libraries(rlottie PRIVATE "-Wl,--version-script=.../rlottie.expmap")
# and that script ends with `local: *;` — it exports only `lottie_*` and
# `rlottie::*` and hides everything else.
#
# rlottie is built STATIC here (BUILD_SHARED_LIBS=OFF). CMake propagates a
# static library's PRIVATE link items to consumers via $<LINK_ONLY:...>, because
# they must still be present when the final binary links. So that flag lands on
# OUR shared library's link line and applies rlottie's version script to the
# whole .so — silently hiding every Java_com_rlottie_* entry point and
# JNI_OnLoad. The library then loads fine and fails at the first native call
# with UnsatisfiedLinkError.
#
# Caught by Chunk 3.5's first real link (a -fsyntax-only check cannot see it).
# Strip it from the interface; we define our own export map below.
get_target_property(_rlottie_interface rlottie INTERFACE_LINK_LIBRARIES)
if(_rlottie_interface)
    list(FILTER _rlottie_interface EXCLUDE REGEX "version-script")
    set_target_properties(rlottie PROPERTIES
        INTERFACE_LINK_LIBRARIES "${_rlottie_interface}")
endif()

add_library(react-native-rlottie SHARED
    # JNI layer (Chunk 3.1)
    ${CMAKE_CURRENT_SOURCE_DIR}/RlottieJni.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/JniPlayerHandle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AndroidFrameSink.cpp
    # Shared C++ core (Phase 1)
    ${RN_RLOTTIE_ROOT}/cpp/RlottiePlayerCore.cpp
    ${RN_RLOTTIE_ROOT}/cpp/FrameBuffer.cpp
    ${RN_RLOTTIE_ROOT}/cpp/PlaybackController.cpp
    ${RN_RLOTTIE_ROOT}/cpp/RenderCoordinator.cpp
    ${RN_RLOTTIE_ROOT}/cpp/ModelCacheController.cpp
)

target_include_directories(react-native-rlottie PRIVATE
    ${RN_RLOTTIE_ROOT}/cpp
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Export ONLY the JNI entry points (see react-native-rlottie.expmap for why).
# --no-undefined turns a missing symbol into a link error here rather than a
# dlopen failure on device.
target_link_options(react-native-rlottie PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/react-native-rlottie.expmap"
    "-Wl,--no-undefined"
)

# jnigraphics provides AndroidBitmap_lockPixels for the front-buffer blit.
target_link_libraries(react-native-rlottie
    PRIVATE
    rlottie::rlottie
    android
    log
    jnigraphics
)
