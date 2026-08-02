# Chunk 1.7 — shared C++ core test target.
#
# Configure with a sanitizer variant:
#   cmake -S tests/cpp -B build/plain
#   cmake -S tests/cpp -B build/asan -DRNRLOTTIE_SANITIZE=asan
#   cmake -S tests/cpp -B build/tsan -DRNRLOTTIE_SANITIZE=tsan
# then `cmake --build build/<v>` and `ctest --test-dir build/<v> --output-on-failure`.
cmake_minimum_required(VERSION 3.18)
project(rnrlottie_tests LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# --- Vendored rlottie (built via its own CMake; plan §14 options) ------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LOTTIE_THREAD  ON  CACHE BOOL "" FORCE)
set(LOTTIE_CACHE   ON  CACHE BOOL "" FORCE)
set(LOTTIE_MODULE  OFF CACHE BOOL "" FORCE)
set(LOTTIE_TEST    OFF CACHE BOOL "" FORCE)
set(LOTTIE_EXAMPLE OFF CACHE BOOL "" FORCE)
add_subdirectory(${REPO}/cpp/third_party/rlottie ${CMAKE_CURRENT_BINARY_DIR}/rlottie)
if(APPLE)
  # Apple clang defines __ARM_NEON__ on arm64, activating rlottie's 32-bit NEON
  # path whose ASM rlottie never builds. Force the generic path (see Chunk 0.3).
  target_compile_options(rlottie PRIVATE -U__ARM_NEON__)
endif()

# --- Sanitizer selection -----------------------------------------------------
set(RNRLOTTIE_SANITIZE "off" CACHE STRING "off | asan | tsan")
set(SAN_FLAGS "")
if(RNRLOTTIE_SANITIZE STREQUAL "asan")
  set(SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer
      -fsanitize-ignorelist=${CMAKE_CURRENT_SOURCE_DIR}/rlottie-ubsan-ignorelist.txt)
elseif(RNRLOTTIE_SANITIZE STREQUAL "tsan")
  set(SAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
endif()
# Sanitize the whole tree (including rlottie) so cross-boundary bugs are caught;
# the ignore list keeps rlottie's benign UBSan finding out of the results.
if(SAN_FLAGS)
  target_compile_options(rlottie PRIVATE ${SAN_FLAGS})
endif()

file(GLOB TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
# fuzz_loadfromdata.cpp defines ONLY LLVMFuzzerTestOneInput (no main()) — it is
# a separate libFuzzer binary (Chunk 5.5, target below), never part of the
# CHECK/TEST binary built from TEST_SOURCES.
list(FILTER TEST_SOURCES EXCLUDE REGEX "/fuzz_[^/]*\\.cpp$")

add_executable(rnrlottie_tests
  ${TEST_SOURCES}
  ${REPO}/cpp/RlottiePlayerCore.cpp
  ${REPO}/cpp/FrameBuffer.cpp
  ${REPO}/cpp/PlaybackController.cpp
  ${REPO}/cpp/RenderCoordinator.cpp
  ${REPO}/cpp/ModelCacheController.cpp
  # Chunk 3.1 — the JNI-free half of the Android adapter (handle safety, the
  # poll-based sink, pixel conversion). RlottieJni.cpp needs the NDK and is
  # compile-checked separately by scripts/check-android-build.sh.
  ${REPO}/android/src/main/cpp/JniPlayerHandle.cpp
  ${REPO}/android/src/main/cpp/AndroidFrameSink.cpp
)
target_include_directories(rnrlottie_tests PRIVATE
  ${REPO}/cpp
  ${REPO}/android/src/main/cpp
)
target_compile_definitions(rnrlottie_tests PRIVATE RNRLOTTIE_TEST_DATA_DIR="${REPO}/tests")
target_compile_options(rnrlottie_tests PRIVATE -Wall -Wextra ${SAN_FLAGS})
target_link_options(rnrlottie_tests PRIVATE ${SAN_FLAGS})
target_link_libraries(rnrlottie_tests PRIVATE rlottie::rlottie)

enable_testing()
add_test(NAME rnrlottie_tests COMMAND rnrlottie_tests)

# --- Chunk 5.5: libFuzzer target over loadFromData/renderFrame ---------------
#
# -fsanitize=fuzzer needs Clang's fuzzer runtime; only offered there. Not added
# to `add_test` / the default `ctest` run (that stays fast — see run-tests.sh);
# invoke tests/run-tests.sh fuzz for a bounded smoke run, or run the binary
# directly for a real campaign.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_executable(rnrlottie_fuzz_loadfromdata
    fuzz_loadfromdata.cpp
    ${REPO}/cpp/RlottiePlayerCore.cpp
    ${REPO}/cpp/FrameBuffer.cpp
  )
  target_include_directories(rnrlottie_fuzz_loadfromdata PRIVATE ${REPO}/cpp)
  set(FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
      -fsanitize-ignorelist=${CMAKE_CURRENT_SOURCE_DIR}/rlottie-ubsan-ignorelist.txt)
  target_compile_options(rnrlottie_fuzz_loadfromdata PRIVATE -Wall -Wextra ${FUZZ_FLAGS})
  target_link_options(rnrlottie_fuzz_loadfromdata PRIVATE ${FUZZ_FLAGS})
  target_link_libraries(rnrlottie_fuzz_loadfromdata PRIVATE rlottie::rlottie)
endif()
