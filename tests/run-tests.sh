#!/usr/bin/env bash
#
# Chunk 1.7 — shared C++ core test driver.
#
# Runs the test target in one or more variants: plain, asan (ASan+UBSan), tsan.
#   tests/run-tests.sh              # all three
#   tests/run-tests.sh plain asan   # a subset
#
# Uses CMake + CTest when available; otherwise falls back to a direct clang
# build with equivalent flags (so it runs without CMake installed).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-clang++}"
VARIANTS=("$@")
if [ "${#VARIANTS[@]}" -eq 0 ]; then
  VARIANTS=(plain asan tsan)
fi

run_cmake() {
  local v="$1" san bdir
  case "$v" in
    plain) san="off" ;;
    asan)  san="asan" ;;
    tsan)  san="tsan" ;;
    *) echo "unknown variant: $v" >&2; return 2 ;;
  esac
  bdir="$ROOT/build/tests-$v"
  echo "== [cmake:$v] configure/build =="
  cmake -S "$ROOT/tests/cpp" -B "$bdir" -DRNRLOTTIE_SANITIZE="$san" >/dev/null
  cmake --build "$bdir" -j >/dev/null
  ctest --test-dir "$bdir" --output-on-failure
}

# --- Fallback: direct clang build (no CMake) --------------------------------
rlottie_srcs() {
  find "$ROOT/cpp/third_party/rlottie/src" -name '*.cpp' \
    -not -path '*/wasm/*' -not -path '*/binding/c/*' | sort
}

INC=(
  -I"$ROOT/cpp/third_party/rlottie/inc"
  -I"$ROOT/cpp/third_party/rlottie/src/vector"
  -I"$ROOT/cpp/third_party/rlottie/src/vector/freetype"
  -I"$ROOT/cpp/third_party/rlottie/src/vector/pixman"
  -I"$ROOT/cpp/third_party/rlottie/src/vector/stb"
  -I"$ROOT/cpp/third_party/rlottie/src/lottie"
  -I"$ROOT/cpp/third_party/rlottie_config"
  -I"$ROOT/cpp"
  -I"$ROOT/android/src/main/cpp"
  -I"$ROOT/tests/cpp"
)

run_fallback() {
  local v="$1" san=() rlsan=() bdir rldir
  case "$v" in
    plain) san=() ; rlsan=() ;;
    asan)  san=(-fsanitize=address,undefined -fno-omit-frame-pointer
               -fsanitize-ignorelist="$ROOT/tests/cpp/rlottie-ubsan-ignorelist.txt")
           rlsan=("${san[@]}") ;;
    tsan)  san=(-fsanitize=thread -fno-omit-frame-pointer) ; rlsan=("${san[@]}") ;;
    *) echo "unknown variant: $v" >&2; return 2 ;;
  esac
  bdir="$ROOT/build/fallback-$v"; rldir="$bdir/rlottie"
  mkdir -p "$rldir"

  # Cache rlottie objects per variant.
  if [ ! -f "$rldir/.done" ]; then
    echo "== [clang:$v] compiling vendored rlottie =="
    local i=0 f
    while IFS= read -r f; do
      "$CXX" -std=c++14 -fno-exceptions -fno-rtti -U__ARM_NEON__ -DNDEBUG -O1 -g \
        ${rlsan[@]+"${rlsan[@]}"} "${INC[@]}" -c "$f" -o "$rldir/rl_$((i++)).o"
    done < <(rlottie_srcs)
    touch "$rldir/.done"
  fi

  echo "== [clang:$v] compiling core + tests =="
  local objs=() src o
  for src in \
    "$ROOT"/cpp/RlottiePlayerCore.cpp "$ROOT"/cpp/FrameBuffer.cpp \
    "$ROOT"/cpp/PlaybackController.cpp "$ROOT"/cpp/RenderCoordinator.cpp \
    "$ROOT"/cpp/ModelCacheController.cpp \
    "$ROOT"/android/src/main/cpp/JniPlayerHandle.cpp \
    "$ROOT"/android/src/main/cpp/AndroidFrameSink.cpp \
    "$ROOT"/tests/cpp/*.cpp; do
    o="$bdir/$(basename "$src").o"
    "$CXX" -std=c++17 -Wall -Wextra -Werror ${san[@]+"${san[@]}"} "${INC[@]}" \
      -DRNRLOTTIE_TEST_DATA_DIR="\"$ROOT/tests\"" -c "$src" -o "$o"
    objs+=("$o")
  done
  "$CXX" ${san[@]+"${san[@]}"} "$rldir"/rl_*.o "${objs[@]}" -o "$bdir/rnrlottie_tests" -lpthread
  echo "== [clang:$v] run =="
  ( cd "$ROOT" && "$bdir/rnrlottie_tests" )
}

status=0
for v in "${VARIANTS[@]}"; do
  echo
  echo "############### variant: $v ###############"
  if command -v cmake >/dev/null 2>&1; then
    run_cmake "$v" || status=1
  else
    run_fallback "$v" || status=1
  fi
done

echo
if [ "$status" -eq 0 ]; then echo "ALL VARIANTS PASSED"; else echo "SOME VARIANTS FAILED"; fi
exit "$status"
