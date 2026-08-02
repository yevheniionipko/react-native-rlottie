#!/usr/bin/env bash
#
# Chunk 3.1 — compile-check the JNI layer against the real NDK headers.
#
# RlottieJni.cpp includes <jni.h> and <android/bitmap.h>, so the host test suite
# (tests/run-tests.sh) cannot build it — it covers the JNI-free logic instead.
# This script type-checks the JNI translation unit itself for every shipped ABI,
# catching signature/API drift without an emulator. A full .so link is Chunk 3.5
# (Gradle + externalNativeBuild).
#
#   scripts/check-android-build.sh [<api-level>]
#
# Uses $ANDROID_NDK_HOME, else the newest NDK under $ANDROID_HOME/ndk.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
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
