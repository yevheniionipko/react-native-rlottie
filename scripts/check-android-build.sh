#!/usr/bin/env bash
#
# Chunk 3.1/3.5 — check the Android native build against the real NDK.
#
#   scripts/check-android-build.sh [<api-level>]           # syntax-only (fast)
#   scripts/check-android-build.sh --link [<api-level>]    # real .so link
#
# Two modes, because the fast one is genuinely not sufficient:
#
#   default  Type-checks each JNI translation unit (-fsyntax-only) for every
#            shipped ABI. Catches signature/API drift in seconds.
#
#   --link   Actually configures CMake and LINKS libreact-native-rlottie.so per
#            ABI, then asserts the Java_com_rlottie_* symbols are exported.
#            Slower (it builds all of rlottie), and it is the only mode that can
#            catch link-time and symbol-visibility faults. Chunk 3.5 found a real
#            one this way: rlottie's version script was propagating onto our link
#            line and hiding every JNI entry point — a syntax-only check passes
#            happily while the library fails at the first native call on device.
#
# Uses $ANDROID_NDK_HOME, else the newest NDK under $ANDROID_HOME/ndk.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="syntax"
if [ "${1:-}" = "--link" ]; then
  MODE="link"
  shift
fi
API="${1:-21}"

NDK="${ANDROID_NDK_HOME:-}"
if [ -z "$NDK" ]; then
  SDK="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
  if [ -d "$SDK/ndk" ]; then
    NDK="$SDK/ndk/$(ls "$SDK/ndk" | sort -V | tail -1)"
  fi
fi
if [ ! -d "$NDK" ]; then
  echo "no NDK found (set ANDROID_NDK_HOME)" >&2
  exit 2
fi

HOST_TAG="darwin-x86_64"
case "$(uname -s)" in
  Linux) HOST_TAG="linux-x86_64" ;;
esac
BIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin"
if [ ! -d "$BIN" ]; then
  echo "unexpected NDK layout: $BIN missing" >&2
  exit 2
fi

echo "NDK: $NDK (api $API)"
OUT="$ROOT/build/android-syntax"
mkdir -p "$OUT"

status=0

# --- --link: configure + build + assert exported symbols, per ABI ------------
if [ "$MODE" = "link" ]; then
  SDK_ROOT="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
  CMAKE="$(ls -d "$SDK_ROOT"/cmake/*/bin/cmake 2>/dev/null | sort -V | tail -1)"
  NINJA="$(ls -d "$SDK_ROOT"/cmake/*/bin/ninja 2>/dev/null | sort -V | tail -1)"
  CMAKE="${CMAKE:-$(command -v cmake || true)}"
  if [ -z "$CMAKE" ]; then
    echo "no cmake found (looked in \$ANDROID_HOME/cmake and \$PATH)" >&2
    exit 2
  fi
  READELF="$BIN/llvm-readelf"

  for ABI in arm64-v8a x86_64 armeabi-v7a; do
    BDIR="$ROOT/build/so-$ABI"
    echo "== $ABI: configure + link"
    rm -rf "$BDIR"
    if ! "$CMAKE" -S "$ROOT/android/src/main/cpp" -B "$BDIR" -G Ninja \
        ${NINJA:+-DCMAKE_MAKE_PROGRAM="$NINJA"} \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" \
        -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release > "$BDIR.cfg.log" 2>&1; then
      echo "   CONFIGURE FAILED (see $BDIR.cfg.log)"; status=1; continue
    fi
    if ! "$CMAKE" --build "$BDIR" -j > "$BDIR.build.log" 2>&1; then
      echo "   LINK FAILED (see $BDIR.build.log)"
      grep -E "error:" "$BDIR.build.log" | head -5
      status=1; continue
    fi
    SO="$BDIR/libreact-native-rlottie.so"
    # The point of this mode: a .so that links but exports nothing is a runtime
    # UnsatisfiedLinkError, so assert the JNI entry points are actually visible.
    N="$("$READELF" --dyn-symbols "$SO" 2>/dev/null | grep -c "Java_com_rlottie" || true)"
    if [ "$N" -eq 0 ]; then
      echo "   LINKED but exports NO Java_com_rlottie_* symbols — check the version script"
      status=1
    else
      echo "   linked, $N JNI symbols exported"
    fi
  done

  if [ "$status" -eq 0 ]; then echo "ANDROID NATIVE LINK OK"; else echo "ANDROID NATIVE LINK FAILED"; fi
  exit "$status"
fi

for TRIPLE in aarch64-linux-android x86_64-linux-android armv7a-linux-androideabi; do
  CXX="$BIN/${TRIPLE}${API}-clang++"
  if [ ! -x "$CXX" ]; then
    echo "== skip $TRIPLE (no compiler at api $API)"
    continue
  fi
  echo "== $TRIPLE"
  for SRC in RlottieJni.cpp JniPlayerHandle.cpp AndroidFrameSink.cpp; do
    "$CXX" -std=c++17 -Wall -Wextra -Werror -fsyntax-only \
      -I"$ROOT/cpp" -I"$ROOT/android/src/main/cpp" \
      "$ROOT/android/src/main/cpp/$SRC" || status=1
  done
done

if [ "$status" -eq 0 ]; then echo "ANDROID JNI SOURCES OK"; else echo "ANDROID JNI CHECK FAILED"; fi
exit "$status"
