# react-native-rlottie — Android native build (Chunk 0.3, minimal wiring).
# Builds the shared library that links the vendored rlottie static lib. The full
# JNI/core source set is added in Phase 1 (core) and Chunk 3.x (JNI).
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

add_library(react-native-rlottie SHARED
    ${RN_RLOTTIE_ROOT}/cpp/spike/probe.cpp
)

target_include_directories(react-native-rlottie PRIVATE
    ${RN_RLOTTIE_ROOT}/cpp
)

# jnigraphics is linked in Chunk 3.5 (Bitmap presenter); not needed for the probe.
target_link_libraries(react-native-rlottie
    PRIVATE
    rlottie::rlottie
    android
    log
)
