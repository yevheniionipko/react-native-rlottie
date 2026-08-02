#!/usr/bin/env bash
#
# Chunk 7.4 (Part C) — builds tests/device/android/golden_runner.cpp as a
# plain native Android executable (no JNI, no APK, no gradle — it only needs
# RlottiePlayerCore + vendored rlottie, cross-compiled with the NDK) and runs
# all three golden-verification legs against a BOOTED emulator/device:
#
#   1. core leg        (native, on-device): render every fixture/golden frame
#                       through the real RlottiePlayerCore and compare
#                       byte-exact against the committed golden.
#   2. conversion leg   (native, on-device): run every golden frame through the
#                       shipped rlottieSurfaceToAndroid() into a padded-stride
#                       destination, assert channel order + untouched padding
#                       + lossless round trip, and write a tightly-packed RGBA
#                       copy + manifest for leg 3.
#   3. real-Bitmap leg (BitmapGoldenMain.java, under app_process — no gradle,
#                       no APK): loads those RGBA bytes into a REAL
#                       android.graphics.Bitmap and checks getPixel().
#
# Exit code is non-zero if the device/NDK/SDK aren't available, if any build
# step fails, or if any of the three legs reports a failure — this is meant to
# FAIL LOUDLY rather than silently no-op (tests/run-tests.sh's `leaks` variant
# precedent: refuse to report success when the thing it depends on is
# missing).
#
# Usage: scripts/run-golden-android.sh
#
# Recognized env overrides (all optional — auto-detected otherwise):
#   ANDROID_HOME / ANDROID_SDK_ROOT   SDK root (default ~/Library/Android/sdk)
#   RNRLOTTIE_NDK_API                 NDK per-API clang wrapper suffix (default 24,
#                                     matching the recipe verified for this repo;
#                                     an OLDER API number than the device's own
#                                     is fine — it only sets the MINIMUM API the
#                                     binary requires, not what it runs on)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NDK_API="${RNRLOTTIE_NDK_API:-24}"
ANDROID_SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"

fail() {
  echo "ERROR: $*" >&2
  exit 1
}

echo "== [android-golden] locating SDK/NDK/adb/device =="

[ -d "$ANDROID_SDK" ] || fail "Android SDK not found at '$ANDROID_SDK'. Set ANDROID_HOME/ANDROID_SDK_ROOT."

command -v adb >/dev/null 2>&1 || {
  if [ -x "$ANDROID_SDK/platform-tools/adb" ]; then
    export PATH="$ANDROID_SDK/platform-tools:$PATH"
  else
    fail "adb not found (not on PATH and not at \$ANDROID_SDK/platform-tools/adb)."
  fi
}

# Refuse to silently skip when there's no booted device — this script's whole
# reason to exist is running on real hardware/emulator, so "no device" is a
# hard failure, never a quiet pass.
DEVICE_LINE="$(adb devices | awk 'NR>1 && $2=="device" {print; exit}')"
[ -n "$DEVICE_LINE" ] || fail "no booted Android device/emulator visible to 'adb devices'. Boot one first."
DEVICE_SERIAL="$(echo "$DEVICE_LINE" | awk '{print $1}')"
echo "   device: $DEVICE_SERIAL"

NDK_DIR="$(find "$ANDROID_SDK/ndk" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -V | tail -1)"
[ -n "$NDK_DIR" ] && [ -d "$NDK_DIR" ] || fail "no NDK found under '$ANDROID_SDK/ndk'."
echo "   NDK: $NDK_DIR"

case "$(uname -s)" in
  Darwin) HOST_TAG="darwin-x86_64" ;;
  Linux)  HOST_TAG="linux-x86_64" ;;
  *) fail "unsupported host OS for NDK toolchain selection: $(uname -s)" ;;
esac
TOOLCHAIN_BIN="$NDK_DIR/toolchains/llvm/prebuilt/$HOST_TAG/bin"
[ -d "$TOOLCHAIN_BIN" ] || fail "NDK toolchain bin not found at '$TOOLCHAIN_BIN'."

BUILD_TOOLS_DIR="$(find "$ANDROID_SDK/build-tools" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -V | tail -1)"
[ -n "$BUILD_TOOLS_DIR" ] || fail "no build-tools found under '$ANDROID_SDK/build-tools'."
D8="$BUILD_TOOLS_DIR/d8"
[ -x "$D8" ] || fail "d8 not found/executable at '$D8'."
echo "   build-tools: $BUILD_TOOLS_DIR"

# Prefer android-36 (the platform jar this repo's recipe was verified against
# for LEG 3's javac/d8 steps, including against a higher-API device — an
# android.jar's API level only bounds which SDK symbols compile against, not
# which device the resulting dex can run on), else fall back to the newest
# platform actually installed.
if [ -f "$ANDROID_SDK/platforms/android-36/android.jar" ]; then
  ANDROID_JAR="$ANDROID_SDK/platforms/android-36/android.jar"
else
  ANDROID_JAR="$(find "$ANDROID_SDK/platforms" -mindepth 1 -maxdepth 1 -type d 2>/dev/null \
    | sort -V | tail -1)/android.jar"
fi
[ -f "$ANDROID_JAR" ] || fail "no android.jar found under '$ANDROID_SDK/platforms'."
echo "   android.jar: $ANDROID_JAR"

# Read the ABI from the device itself — never hardcode arm64. Also read the
# device's own API level purely for the log; the NDK API level used to COMPILE
# (NDK_API above) is independent and only sets the binary's minimum-API floor.
ABI="$(adb -s "$DEVICE_SERIAL" shell getprop ro.product.cpu.abi | tr -d '\r\n ')"
DEVICE_API="$(adb -s "$DEVICE_SERIAL" shell getprop ro.build.version.sdk | tr -d '\r\n ')"
[ -n "$ABI" ] || fail "could not read ro.product.cpu.abi from device."
echo "   device ABI: $ABI  (device API level: $DEVICE_API, compiling for NDK API $NDK_API)"

case "$ABI" in
  arm64-v8a)    TRIPLE="aarch64-linux-android" ;;
  armeabi-v7a)  TRIPLE="armv7a-linux-androideabi" ;;
  x86_64)       TRIPLE="x86_64-linux-android" ;;
  x86)          TRIPLE="i686-linux-android" ;;
  *) fail "unsupported/unknown device ABI '$ABI'." ;;
esac

CXX="$TOOLCHAIN_BIN/${TRIPLE}${NDK_API}-clang++"
[ -x "$CXX" ] || fail "expected NDK compiler not found/executable at '$CXX' (NDK $NDK_DIR, API $NDK_API)."
echo "   compiler: $CXX"

# --- Compile the golden_runner native executable ----------------------------
#
# Same -I set as tests/run-tests.sh's fallback build (rlottie's own headers +
# cpp/ + android/src/main/cpp/ for AndroidPixelConvert.h + tests/cpp/ for
# GoldenFixtures.h/GoldenCompare.h) so this stays on the exact same code path
# as the host gate, just cross-compiled.
#
# TRAP (already hit once building this): zsh does NOT word-split an unquoted
# $VAR the way bash does, so a plain `$INC_STR` with space-separated -I flags
# collapses into one argument and every include silently fails to resolve.
# Use bash arrays (this script runs under bash via the shebang) and always
# expand with "${INC[@]}".
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

BUILD_DIR="$ROOT/build/android-golden-$ABI"
RL_DIR="$BUILD_DIR/rlottie"
mkdir -p "$RL_DIR"

# armeabi-v7a needs -U__ARM_NEON__ (see android/src/main/cpp/CMakeLists.txt's
# long comment on why: the NDK defines __ARM_NEON__ there but never assembles
# pixman's NEON .S file, so leaving it defined is a link-time undefined symbol).
# arm64-v8a — the ABI that actually matters for a real device/emulator — is
# unaffected and keeps its own path, so this only fires on that one ABI.
NEON_FLAG=()
[ "$ABI" = "armeabi-v7a" ] && NEON_FLAG=(-U__ARM_NEON__)

rlottie_srcs() {
  find "$ROOT/cpp/third_party/rlottie/src" -name '*.cpp' \
    -not -path '*/wasm/*' -not -path '*/binding/c/*' | sort
}

if [ ! -f "$RL_DIR/.done" ]; then
  echo "== [android-golden:$ABI] compiling vendored rlottie (cached under $RL_DIR) =="
  i=0
  while IFS= read -r f; do
    "$CXX" -std=c++14 -fno-exceptions -fno-rtti ${NEON_FLAG[@]+"${NEON_FLAG[@]}"} -DNDEBUG -O2 \
      "${INC[@]}" -c "$f" -o "$RL_DIR/rl_$((i++)).o"
  done < <(rlottie_srcs)
  touch "$RL_DIR/.done"
else
  echo "== [android-golden:$ABI] vendored rlottie objects cached, skipping recompile =="
fi

echo "== [android-golden:$ABI] compiling RlottiePlayerCore + golden_runner =="
CORE_OBJS=()
for src in "$ROOT/cpp/RlottiePlayerCore.cpp" "$ROOT/tests/device/android/golden_runner.cpp"; do
  o="$BUILD_DIR/$(basename "$src").o"
  "$CXX" -std=c++17 -Wall -Wextra "${INC[@]}" -c "$src" -o "$o"
  CORE_OBJS+=("$o")
done

echo "== [android-golden:$ABI] linking golden_runner =="
# -static-libstdc++ statically links the NDK's libc++ (there is no libstdc++ on
# Android; the NDK driver maps this flag onto libc++ appropriately) so we don't
# have to push libc++_shared.so alongside the binary — verified on this NDK.
# No -lpthread: bionic's libc already provides pthread symbols, and the NDK
# ships only an empty compatibility stub for a separate libpthread.
"$CXX" -static-libstdc++ -pie "$RL_DIR"/rl_*.o "${CORE_OBJS[@]}" \
  -o "$BUILD_DIR/golden_runner"

# --- Compile the LEG 3 Java entry point -------------------------------------
echo "== [android-golden] compiling BitmapGoldenMain.java (javac + d8, no gradle/APK) =="
JAVA_BUILD="$BUILD_DIR/java"
CLASSES_DIR="$JAVA_BUILD/classes"
mkdir -p "$CLASSES_DIR"
javac --release 17 -cp "$ANDROID_JAR" -d "$CLASSES_DIR" \
  "$ROOT/tests/device/android/BitmapGoldenMain.java"
# Include every .class file, not just BitmapGoldenMain.class: nested/inner
# classes (BitmapGoldenMain$ManifestEntry) are separate .class files sharing a
# nest, and d8 refuses to compile one without its nest mates on the path.
"$D8" --lib "$ANDROID_JAR" --output "$JAVA_BUILD" "$CLASSES_DIR"/*.class
[ -f "$JAVA_BUILD/classes.dex" ] || fail "d8 did not produce classes.dex at '$JAVA_BUILD'."

# --- Push fixtures/goldens/binaries to the device ---------------------------
REMOTE_BASE="/data/local/tmp/rnrlottie-golden"
REMOTE_DATA="$REMOTE_BASE/data"
REMOTE_OUT="$REMOTE_BASE/out"
REMOTE_BIN="$REMOTE_BASE/golden_runner"
REMOTE_DEX="$REMOTE_BASE/golden.dex"

ADB="adb -s $DEVICE_SERIAL"

echo "== [android-golden] pushing fixtures/goldens/binary to $REMOTE_BASE =="
$ADB shell "mkdir -p $REMOTE_DATA/fixtures/golden $REMOTE_DATA/golden $REMOTE_OUT"
$ADB push "$ROOT"/tests/fixtures/golden/*.json "$REMOTE_DATA/fixtures/golden/" >/dev/null
$ADB push "$ROOT"/tests/golden/*.argb32.raw "$REMOTE_DATA/golden/" >/dev/null
$ADB push "$BUILD_DIR/golden_runner" "$REMOTE_BIN" >/dev/null
$ADB shell "chmod 755 $REMOTE_BIN"
$ADB push "$JAVA_BUILD/classes.dex" "$REMOTE_DEX" >/dev/null

status=0

# --- Leg 1 + Leg 2: the native runner ---------------------------------------
echo
echo "############### LEG 1 + 2: native core + conversion (on-device) ###############"
if ! $ADB shell "RNRLOTTIE_GOLDEN_DIR=$REMOTE_DATA $REMOTE_BIN $REMOTE_OUT"; then
  echo "!! native golden_runner reported failure (legs 1/2)" >&2
  status=1
fi

# --- Leg 3: the real Bitmap check under app_process -------------------------
echo
echo "############### LEG 3: real android.graphics.Bitmap (app_process) ###############"
if ! $ADB shell "CLASSPATH=$REMOTE_DEX app_process $REMOTE_BASE BitmapGoldenMain $REMOTE_OUT"; then
  echo "!! BitmapGoldenMain reported failure (leg 3)" >&2
  status=1
fi

echo
if [ "$status" -eq 0 ]; then
  echo "ALL ANDROID GOLDEN LEGS PASSED"
else
  echo "SOME ANDROID GOLDEN LEGS FAILED"
fi
exit "$status"
