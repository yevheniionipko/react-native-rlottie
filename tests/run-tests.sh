#!/usr/bin/env bash
#
# Chunk 1.7 / 5.5 — shared C++ core test driver.
#
# Runs the test target in one or more variants: plain, asan (ASan+UBSan), tsan.
#   tests/run-tests.sh              # all three (the default gate; stays fast)
#   tests/run-tests.sh plain asan   # a subset
#
# A fourth, OPT-IN variant, "fuzz", builds and runs the Chunk 5.5 libFuzzer
# target (tests/cpp/fuzz_loadfromdata.cpp) over loadFromData/renderFrame for a
# bounded smoke run — NOT an unbounded fuzz session, and NOT part of the
# default variant list, so it never slows down the normal test gate:
#   tests/run-tests.sh fuzz         # ~20s bounded smoke run
#   tests/run-tests.sh plain fuzz   # combine with the normal suite
#
# A fifth, OPT-IN variant, "bench" (Chunk 7.2), builds and runs
# tests/cpp/bench_render.cpp — renderSync() vs rlottie's async
# render()/std::future, unsanitized (a benchmark under ASan/TSan measures the
# sanitizer, not rlottie). Also not part of the default variant list — see
# docs/render-benchmark.md for methodology and results:
#   tests/run-tests.sh bench        # ~10-20s, prints raw numbers
#
# A sixth, OPT-IN variant, "leaks" (Chunk 7.3), is the actual LEAK-CLEAN gate
# — see docs/lifecycle-testing.md for why this is separate from `asan`:
# ASan's LeakSanitizer does NOT work on macOS (detect_leaks=1 prints "not
# supported on this platform" and exits 0), so the `asan` variant proves
# memory-safety, NOT leak-freedom. On Darwin this variant builds the PLAIN
# (unsanitized — `leaks` and ASan's allocator conflict) test binary and runs
# it under `/usr/bin/leaks --atExit`, which DOES work as an at-exit leak gate
# on macOS. On a non-Darwin host there is no `leaks` equivalent used here, so
# it falls back to ASan's LeakSanitizer (detect_leaks=1) — real coverage
# there, since LSan works on Linux — rather than silently skipping:
#   tests/run-tests.sh leaks        # ~15-20s on Darwin
#
# Uses CMake + CTest when available; otherwise falls back to a direct clang
# build with equivalent flags (so it runs without CMake installed).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-clang++}"
# Bounded smoke-run budget for the `fuzz` variant (seconds). Override with
# RNRLOTTIE_FUZZ_SECONDS for a longer local campaign; CI/the default gate never
# sets it, so it stays at this short smoke value.
FUZZ_SECONDS="${RNRLOTTIE_FUZZ_SECONDS:-20}"
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

# Compiles (but does not run) the fallback test binary for variant `v`, and
# sets the global FALLBACK_BIN to its path. Split out of run_fallback (below)
# so run_leaks (the `leaks` variant) can build the same PLAIN binary and run
# it under `leaks --atExit` instead of executing it directly.
build_fallback() {
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
  # fuzz_loadfromdata.cpp / fuzz_standalone_driver.cpp are a SEPARATE binary
  # (Chunk 5.5, built by run_fuzz_fallback): they don't define main() the way
  # test_harness.cpp does (fuzz_loadfromdata.cpp has none at all; the driver's
  # own main() would collide), so they're excluded from this glob.
  # bench_render.cpp (Chunk 7.2, built by run_bench_fallback) defines its own
  # main() too and is excluded for the same reason.
  for src in \
    "$ROOT"/cpp/RlottiePlayerCore.cpp "$ROOT"/cpp/FrameBuffer.cpp \
    "$ROOT"/cpp/PlaybackController.cpp "$ROOT"/cpp/RenderCoordinator.cpp \
    "$ROOT"/cpp/ModelCacheController.cpp \
    "$ROOT"/android/src/main/cpp/JniPlayerHandle.cpp \
    "$ROOT"/android/src/main/cpp/AndroidFrameSink.cpp \
    $(find "$ROOT/tests/cpp" -maxdepth 1 -name '*.cpp' -not -name 'fuzz_*' -not -name 'bench_*'); do
    o="$bdir/$(basename "$src").o"
    "$CXX" -std=c++17 -Wall -Wextra -Werror ${san[@]+"${san[@]}"} "${INC[@]}" \
      -DRNRLOTTIE_TEST_DATA_DIR="\"$ROOT/tests\"" -c "$src" -o "$o"
    objs+=("$o")
  done
  "$CXX" ${san[@]+"${san[@]}"} "$rldir"/rl_*.o "${objs[@]}" -o "$bdir/rnrlottie_tests" -lpthread
  FALLBACK_BIN="$bdir/rnrlottie_tests"
}

run_fallback() {
  local v="$1"
  build_fallback "$v"
  echo "== [clang:$v] run =="
  ( cd "$ROOT" && "$FALLBACK_BIN" )
}

# --- fuzz: build + bounded smoke run of the Chunk 5.5 libFuzzer target ------
#
# A dedicated path (not folded into run_cmake/run_fallback above) because it
# builds a SEPARATE binary (libFuzzer supplies its own main(), which would
# collide with test_harness.cpp's) and "success" means "ran for the budgeted
# time/iterations without ASan/UBSan/libFuzzer flagging anything" rather than
# "printed ALL N TESTS PASSED".
seed_fuzz_corpus() {
  local corpus="$1"
  mkdir -p "$corpus"
  cp "$ROOT"/tests/malformed/*.json "$corpus"/ 2>/dev/null || true
  # A tiny valid seed too, so the fuzzer starts from "parses" as well as
  # "rejected" — a from-scratch random-bytes-only corpus rarely gets past the
  # first structural JSON token.
  cp "$ROOT"/tests/fixtures/pixel-probe.json "$corpus"/ 2>/dev/null || true
}

run_fuzz_cmake() {
  local bdir="$ROOT/build/tests-fuzz"
  local corpus="$ROOT/build/fuzz-corpus"
  echo "== [cmake:fuzz] configure/build =="
  cmake -S "$ROOT/tests/cpp" -B "$bdir" -DRNRLOTTIE_SANITIZE=off >/dev/null
  cmake --build "$bdir" -j --target rnrlottie_fuzz_loadfromdata >/dev/null
  seed_fuzz_corpus "$corpus"
  echo "== [cmake:fuzz] smoke run (${FUZZ_SECONDS}s) =="
  ( cd "$ROOT" && "$bdir/rnrlottie_fuzz_loadfromdata" \
      -max_total_time="$FUZZ_SECONDS" -rss_limit_mb=2048 -print_final_stats=1 "$corpus" )
}

run_fuzz_fallback() {
  local bdir="$ROOT/build/fallback-fuzz"
  local rldir="$bdir/rlottie"
  local corpus="$ROOT/build/fuzz-corpus"
  # -fsanitize=fuzzer needs Clang's libFuzzer runtime (libclang_rt.fuzzer_*),
  # part of a full LLVM/compiler-rt install. It compiles fine without it, but
  # FAILS AT LINK. Some clang installs (notably the Xcode Command Line Tools'
  # bundled clang++ in this dev environment) don't ship that archive at all.
  # Try the real thing first; if the link fails, fall back to the standalone
  # driver (fuzz_standalone_driver.cpp) — still ASan+UBSan-instrumented, just
  # without libFuzzer's coverage-guided engine. Either way this function
  # produces a runnable bounded smoke run.
  local fuzzsan=(-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
                 -fsanitize-ignorelist="$ROOT/tests/cpp/rlottie-ubsan-ignorelist.txt")
  local plainsan=(-fsanitize=address,undefined -fno-omit-frame-pointer
                  -fsanitize-ignorelist="$ROOT/tests/cpp/rlottie-ubsan-ignorelist.txt")
  mkdir -p "$rldir"

  if [ ! -f "$rldir/.done" ]; then
    echo "== [clang:fuzz] compiling vendored rlottie =="
    local i=0 f
    while IFS= read -r f; do
      "$CXX" -std=c++14 -fno-exceptions -fno-rtti -U__ARM_NEON__ -DNDEBUG -O1 -g \
        "${plainsan[@]}" "${INC[@]}" -c "$f" -o "$rldir/rl_$((i++)).o"
    done < <(rlottie_srcs)
    touch "$rldir/.done"
  fi

  echo "== [clang:fuzz] compiling core + fuzz entry point =="
  local core_objs=() src o
  for src in "$ROOT"/cpp/RlottiePlayerCore.cpp "$ROOT"/cpp/FrameBuffer.cpp; do
    o="$bdir/$(basename "$src").o"
    "$CXX" -std=c++17 -Wall -Wextra -Werror "${plainsan[@]}" "${INC[@]}" -c "$src" -o "$o"
    core_objs+=("$o")
  done

  local entry_obj="$bdir/fuzz_loadfromdata.cpp.o"
  local mode="libfuzzer"
  if "$CXX" -std=c++17 -Wall -Wextra -Werror "${fuzzsan[@]}" "${INC[@]}" \
      -c "$ROOT/tests/cpp/fuzz_loadfromdata.cpp" -o "$entry_obj" \
      && "$CXX" "${fuzzsan[@]}" "$rldir"/rl_*.o "${core_objs[@]}" "$entry_obj" \
           -o "$bdir/rnrlottie_fuzz_loadfromdata" -lpthread 2>"$bdir/link.log"; then
    :  # real libFuzzer binary linked successfully
  else
    mode="standalone"
    echo "   (no libFuzzer runtime available — falling back to the standalone driver)"
    tail -5 "$bdir/link.log" 2>/dev/null | sed 's/^/   /'
    "$CXX" -std=c++17 -Wall -Wextra -Werror "${plainsan[@]}" "${INC[@]}" \
      -c "$ROOT/tests/cpp/fuzz_loadfromdata.cpp" -o "$entry_obj"
    local driver_obj="$bdir/fuzz_standalone_driver.cpp.o"
    "$CXX" -std=c++17 -Wall -Wextra -Werror "${plainsan[@]}" "${INC[@]}" \
      -c "$ROOT/tests/cpp/fuzz_standalone_driver.cpp" -o "$driver_obj"
    "$CXX" "${plainsan[@]}" "$rldir"/rl_*.o "${core_objs[@]}" "$entry_obj" "$driver_obj" \
      -o "$bdir/rnrlottie_fuzz_loadfromdata" -lpthread
  fi

  seed_fuzz_corpus "$corpus"
  echo "== [clang:fuzz:$mode] smoke run (${FUZZ_SECONDS}s) =="
  if [ "$mode" = "libfuzzer" ]; then
    ( cd "$ROOT" && "$bdir/rnrlottie_fuzz_loadfromdata" \
        -max_total_time="$FUZZ_SECONDS" -rss_limit_mb=2048 -print_final_stats=1 "$corpus" )
  else
    ( cd "$ROOT" && "$bdir/rnrlottie_fuzz_loadfromdata" \
        -max_total_time="$FUZZ_SECONDS" "$corpus"/* )
  fi
}

# --- bench: build + run the Chunk 7.2 renderSync-vs-async benchmark --------
run_bench_cmake() {
  local bdir="$ROOT/build/tests-bench"
  echo "== [cmake:bench] configure/build =="
  cmake -S "$ROOT/tests/cpp" -B "$bdir" -DRNRLOTTIE_SANITIZE=off >/dev/null
  cmake --build "$bdir" -j --target rnrlottie_bench_render >/dev/null
  echo "== [cmake:bench] run =="
  ( cd "$ROOT" && "$bdir/rnrlottie_bench_render" )
}

run_bench_fallback() {
  local bdir="$ROOT/build/fallback-bench"
  local rldir="$bdir/rlottie"
  mkdir -p "$rldir"

  if [ ! -f "$rldir/.done" ]; then
    echo "== [clang:bench] compiling vendored rlottie =="
    local i=0 f
    while IFS= read -r f; do
      "$CXX" -std=c++14 -fno-exceptions -fno-rtti -U__ARM_NEON__ -DNDEBUG -O2 \
        "${INC[@]}" -c "$f" -o "$rldir/rl_$((i++)).o"
    done < <(rlottie_srcs)
    touch "$rldir/.done"
  fi

  echo "== [clang:bench] compiling bench entry point =="
  "$CXX" -std=c++17 -Wall -Wextra -Werror -O2 "${INC[@]}" \
    -DRNRLOTTIE_TEST_DATA_DIR="\"$ROOT/tests\"" \
    -c "$ROOT/tests/cpp/bench_render.cpp" -o "$bdir/bench_render.cpp.o"
  "$CXX" "$rldir"/rl_*.o "$bdir/bench_render.cpp.o" -o "$bdir/rnrlottie_bench_render" -lpthread
  echo "== [clang:bench] run =="
  ( cd "$ROOT" && "$bdir/rnrlottie_bench_render" )
}

# --- leaks: the actual leak-clean gate (Chunk 7.3) --------------------------
#
# See the header comment above for why this is Darwin-`leaks`-based rather
# than folded into `asan`. Builds the PLAIN (unsanitized) binary — `leaks`
# and ASan's allocator implementation conflict, so a build WITH ASan would
# make `leaks`'s findings meaningless — and runs it under `leaks --atExit`,
# which prints "N leaks for M total leaked bytes" and exits 1 on a leaking
# binary, or "0 leaks" and exits 0 on a clean one (verified against both a
# leaking and a non-leaking probe binary).
run_leaks_cmake() {
  local bdir="$ROOT/build/tests-plain"
  echo "== [leaks] configure/build (cmake, plain/unsanitized) =="
  cmake -S "$ROOT/tests/cpp" -B "$bdir" -DRNRLOTTIE_SANITIZE=off >/dev/null
  cmake --build "$bdir" -j --target rnrlottie_tests >/dev/null
  echo "== [leaks] running under leaks --atExit =="
  ( cd "$ROOT" && leaks --atExit -- "$bdir/rnrlottie_tests" )
}

run_leaks_fallback_darwin() {
  build_fallback plain
  echo "== [leaks] running under leaks --atExit =="
  ( cd "$ROOT" && leaks --atExit -- "$FALLBACK_BIN" )
}

run_leaks() {
  if [ "$(uname -s)" != "Darwin" ]; then
    echo "== [leaks] non-Darwin host: no 'leaks' equivalent wired here."
    echo "   Falling back to ASan's LeakSanitizer (detect_leaks=1), which DOES"
    echo "   work on Linux (unlike on macOS) — see docs/lifecycle-testing.md."
    if command -v cmake >/dev/null 2>&1; then
      ASAN_OPTIONS="detect_leaks=1:${ASAN_OPTIONS:-}" run_cmake asan
    else
      ASAN_OPTIONS="detect_leaks=1:${ASAN_OPTIONS:-}" run_fallback asan
    fi
    return $?
  fi
  if ! command -v leaks >/dev/null 2>&1; then
    echo "== [leaks] ERROR: this is Darwin but /usr/bin/leaks is not on PATH."
    echo "   Refusing to silently report success — install Xcode Command"
    echo "   Line Tools (leaks ships with them) or run a different variant."
    return 1
  fi
  if command -v cmake >/dev/null 2>&1; then
    run_leaks_cmake
  else
    run_leaks_fallback_darwin
  fi
}

status=0
for v in "${VARIANTS[@]}"; do
  echo
  echo "############### variant: $v ###############"
  if [ "$v" = "fuzz" ]; then
    if command -v cmake >/dev/null 2>&1; then
      run_fuzz_cmake || status=1
    else
      run_fuzz_fallback || status=1
    fi
  elif [ "$v" = "bench" ]; then
    if command -v cmake >/dev/null 2>&1; then
      run_bench_cmake || status=1
    else
      run_bench_fallback || status=1
    fi
  elif [ "$v" = "leaks" ]; then
    run_leaks || status=1
  elif command -v cmake >/dev/null 2>&1; then
    run_cmake "$v" || status=1
  else
    run_fallback "$v" || status=1
  fi
done

echo
if [ "$status" -eq 0 ]; then echo "ALL VARIANTS PASSED"; else echo "SOME VARIANTS FAILED"; fi
exit "$status"
