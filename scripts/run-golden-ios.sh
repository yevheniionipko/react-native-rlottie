#!/usr/bin/env bash
#
# Chunk 7.4 (Part B) — builds tests/device/ios/golden_runner.mm for the iOS
# Simulator and actually EXECUTES it on a booted simulator via `simctl spawn`.
#
# Unlike tests/run-tests.sh (which proves the core is correct on the host
# machine's clang/arm64), this proves the SAME golden fixtures render
# byte-exact through rlottie on the iOS Simulator runtime, and that the real,
# shipped ios/RNRlottieFramePresenter.mm converts the FrameBuffer front buffer
# into a CGImageRef that reads back correctly through actual CoreGraphics —
# see tests/device/ios/golden_runner.mm's header comment for the two-leg
# design and tests/cpp/GoldenCompare.h for why they get different tolerances.
#
# Usage:
#   scripts/run-golden-ios.sh                # auto-discover/boot a simulator
#   scripts/run-golden-ios.sh --udid <UDID>  # use a specific simulator
#
# `simctl spawn <udid> <path-to-binary>` runs the binary against the HOST
# filesystem (it is not sandboxed the way an installed .app bundle is), so the
# runner reads tests/fixtures/golden/*.json and tests/golden/*.raw in place —
# no bundling/copying step needed. RNRLOTTIE_GOLDEN_DIR pins the data root the
# same way RNRLOTTIE_TEST_DATA_DIR pins it for the host build (GoldenFixtures.h
# checks the env var first, see its own comment).
#
# FAILS LOUDLY (non-zero exit, explicit message) if there is no Simulator
# runtime / SDK / booted device available, rather than silently skipping —
# same policy as tests/run-tests.sh's `leaks` variant on a non-Darwin host.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-clang++}"
UDID=""
IOS_MIN="${RNRLOTTIE_IOS_SIM_MIN:-13.0}"

while [ $# -gt 0 ]; do
  case "$1" in
    --udid) UDID="$2"; shift 2 ;;
    --udid=*) UDID="${1#--udid=}"; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

# --- 1. Find the SDK. Fail loudly if Xcode / the simulator SDK isn't here. --
if ! command -v xcrun >/dev/null 2>&1; then
  echo "ERROR: xcrun not found — this needs a full Xcode install, not just the" >&2
  echo "       Command Line Tools (which lack the iphonesimulator SDK/simctl" >&2
  echo "       simulator runtimes needed to actually RUN this). Refusing to" >&2
  echo "       silently skip." >&2
  exit 1
fi

SDK_PATH="$(xcrun --sdk iphonesimulator --show-sdk-path 2>/dev/null || true)"
if [ -z "$SDK_PATH" ]; then
  echo "ERROR: no iphonesimulator SDK found (xcrun --sdk iphonesimulator" >&2
  echo "       --show-sdk-path failed). Install Xcode (not just CLT) and its" >&2
  echo "       iOS Simulator platform, then retry. Refusing to silently skip." >&2
  exit 1
fi

# --- 2. Find a booted simulator (or boot one) -------------------------------
if [ -z "$UDID" ]; then
  UDID="$(xcrun simctl list devices booted -j 2>/dev/null \
    | /usr/bin/python3 -c '
import json, sys
data = json.load(sys.stdin)
for runtime, devices in data.get("devices", {}).items():
    for d in devices:
        if d.get("state") == "Booted":
            print(d["udid"])
            sys.exit(0)
' 2>/dev/null || true)"
fi

if [ -z "$UDID" ]; then
  echo "No booted simulator found and none specified via --udid." >&2
  echo "Attempting to boot the first available iPhone simulator..." >&2
  UDID="$(xcrun simctl list devices available -j 2>/dev/null \
    | /usr/bin/python3 -c '
import json, sys
data = json.load(sys.stdin)
for runtime, devices in data.get("devices", {}).items():
    if "iOS" not in runtime:
        continue
    for d in devices:
        if "iPhone" in d.get("name", ""):
            print(d["udid"])
            sys.exit(0)
' 2>/dev/null || true)"
  if [ -z "$UDID" ]; then
    echo "ERROR: no available iOS simulator device found at all (checked booted" >&2
    echo "       and available lists via 'xcrun simctl list devices'). This" >&2
    echo "       machine needs at least one iOS Simulator runtime installed." >&2
    echo "       Refusing to silently skip the golden-iOS gate." >&2
    exit 1
  fi
  xcrun simctl boot "$UDID"
  # Booting is async; wait for the device to actually report Booted rather
  # than racing simctl spawn against a still-starting springboard.
  for _ in $(seq 1 30); do
    STATE="$(xcrun simctl list devices -j 2>/dev/null \
      | /usr/bin/python3 -c "
import json, sys
data = json.load(sys.stdin)
for runtime, devices in data.get('devices', {}).items():
    for d in devices:
        if d.get('udid') == '$UDID':
            print(d.get('state', ''))
            sys.exit(0)
" 2>/dev/null || true)"
    [ "$STATE" = "Booted" ] && break
    sleep 1
  done
fi

echo "== using simulator UDID=$UDID =="
xcrun simctl list devices | grep -F "$UDID" || true

# --- 3. Build --------------------------------------------------------------
BDIR="$ROOT/build/ios-golden"
RLDIR="$BDIR/rlottie"
mkdir -p "$RLDIR"

ARCH="arm64"  # this dev environment's host/simulator arch (Apple Silicon).
# An Intel Mac's simulator runs x86_64; detect rather than hardcode so the
# script still works there.
case "$(uname -m)" in
  arm64) ARCH="arm64" ;;
  x86_64) ARCH="x86_64" ;;
  *) echo "WARNING: unrecognized host arch $(uname -m), defaulting to arm64" >&2 ;;
esac

CLANGXX=(xcrun --sdk iphonesimulator "$CXX")

INC=(
  -I"$ROOT/cpp"
  -I"$ROOT/cpp/third_party/rlottie/inc"
  -I"$ROOT/cpp/third_party/rlottie/src/vector"
  -I"$ROOT/cpp/third_party/rlottie/src/vector/freetype"
  -I"$ROOT/cpp/third_party/rlottie/src/vector/pixman"
  -I"$ROOT/cpp/third_party/rlottie/src/vector/stb"
  -I"$ROOT/cpp/third_party/rlottie/src/lottie"
  -I"$ROOT/cpp/third_party/rlottie_config"
  -I"$ROOT/tests/cpp"
  -I"$ROOT/ios"
)

SIM_FLAGS=(-arch "$ARCH" -mios-simulator-version-min="$IOS_MIN")

# --- 3a. Vendored rlottie: same recipe as the podspec's "rlottie" subspec
# (-U__ARM_NEON__ forces the generic render path on Apple-clang arm64, since
# unlike the Android NDK, Apple clang arm64 defines __ARM_NEON__ and would
# otherwise pull in the NEON path this vendored tree never assembles — see
# react-native-rlottie.podspec's own comment). Cached under build/ like
# tests/run-tests.sh does, so re-runs are fast.
rlottie_srcs() {
  find "$ROOT/cpp/third_party/rlottie/src" -name '*.cpp' \
    -not -path '*/wasm/*' -not -path '*/binding/c/*' | sort
}

if [ ! -f "$RLDIR/.done" ]; then
  echo "== [ios-sim:$ARCH] compiling vendored rlottie =="
  i=0
  while IFS= read -r f; do
    "${CLANGXX[@]}" "${SIM_FLAGS[@]}" -std=gnu++14 -fno-exceptions -fno-rtti \
      -U__ARM_NEON__ -DNDEBUG -O2 "${INC[@]}" -c "$f" -o "$RLDIR/rl_$((i++)).o"
  done < <(rlottie_srcs)
  touch "$RLDIR/.done"
fi

# --- 3b. Shared C++ core (exceptions/RTTI enabled — matches the podspec's
# "core" subspec, which does NOT pass -fno-exceptions/-fno-rtti; FrameBuffer.cpp
# relies on catching std::bad_alloc/std::length_error, see cpp/FrameBuffer.cpp).
echo "== [ios-sim:$ARCH] compiling shared C++ core =="
CORE_OBJS=()
for src in "$ROOT/cpp/RlottiePlayerCore.cpp" "$ROOT/cpp/FrameBuffer.cpp"; do
  o="$BDIR/$(basename "$src").o"
  "${CLANGXX[@]}" "${SIM_FLAGS[@]}" -std=c++17 -Wall -Wextra "${INC[@]}" -c "$src" -o "$o"
  CORE_OBJS+=("$o")
done

# --- 3c. The real presenter + this runner (Objective-C++, ARC) --------------
echo "== [ios-sim:$ARCH] compiling RNRlottieFramePresenter + golden_runner =="
OBJC_OBJS=()
for src in "$ROOT/ios/RNRlottieFramePresenter.mm" "$ROOT/tests/device/ios/golden_runner.mm"; do
  o="$BDIR/$(basename "$src").o"
  "${CLANGXX[@]}" "${SIM_FLAGS[@]}" -std=c++17 -fobjc-arc -Wall -Wextra "${INC[@]}" -c "$src" -o "$o"
  OBJC_OBJS+=("$o")
done

echo "== [ios-sim:$ARCH] linking =="
BIN="$BDIR/rnrlottie_ios_golden_runner"
"${CLANGXX[@]}" "${SIM_FLAGS[@]}" -fobjc-arc \
  -framework CoreGraphics -framework Foundation \
  "$RLDIR"/rl_*.o "${CORE_OBJS[@]}" "${OBJC_OBJS[@]}" -o "$BIN"

# --- 4. Run on the simulator, propagating its exit code ---------------------
# `simctl spawn` does NOT forward the calling shell's environment by default;
# it only forwards vars prefixed SIMCTL_CHILD_ (see `xcrun simctl spawn
# --help`'s last line) and strips the prefix for the child. A spawned binary
# sees the HOST filesystem (it is not sandboxed like an installed .app), so
# RNRLOTTIE_GOLDEN_DIR pointing at the host repo path works unmodified.
echo "== [ios-sim] spawning on $UDID =="
set +e
SIMCTL_CHILD_RNRLOTTIE_GOLDEN_DIR="$ROOT/tests" xcrun simctl spawn "$UDID" "$BIN"
STATUS=$?
set -e

echo
if [ "$STATUS" -eq 0 ]; then
  echo "ALL GOLDEN CHECKS PASSED ON SIMULATOR (UDID=$UDID)"
else
  echo "GOLDEN CHECKS FAILED ON SIMULATOR (UDID=$UDID, exit=$STATUS)"
fi
exit "$STATUS"
