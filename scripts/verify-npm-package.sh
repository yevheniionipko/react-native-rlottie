#!/usr/bin/env bash
#
# Chunk 8.1 — npm package content verification.
#
# `npm pack` decides what actually ships from package.json's `files` array,
# and that list has already drifted once silently in this repo's history: it
# listed the whole `android` directory without excluding `android/.cxx`, a
# local CMake build-cache dir (~56 MB of .o/.a build residue) that has no
# business in a published tarball. This script exists to catch that class of
# regression before publish, not just this one instance of it.
#
# Packs a REAL tarball (via `npm pack`, to a scratch dir — never into the
# repo) and asserts:
#   - required files are present (podspec, react-native.config.js, all of
#     src/, the Kotlin sources, android/build.gradle + CMakeLists, the JNI
#     C++, the shared cpp/ core, and — this is the one it's easy to get
#     wrong — the vendored rlottie SOURCE a consumer actually compiles from,
#     not just the third_party/rlottie directory existing but empty of it)
#   - forbidden paths are absent (tests/, docs/, build/, example/,
#     node_modules/, the two top-level implementation-plan docs, .DS_Store,
#     *.o/*.so/*.a build residue)
#   - the license files and THIRD_PARTY_NOTICES.md are included
#   - the unpacked size is reported, so future bloat is visible at a glance
#
# Usage: scripts/verify-npm-package.sh
# Fails loudly (does not skip) if `npm` isn't on PATH or `npm pack` errors.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
status=0
fail() { echo "FAIL: $*" >&2; status=1; }

if ! command -v npm >/dev/null 2>&1; then
  echo "npm not found on PATH — cannot verify the package" >&2
  exit 2
fi

SCRATCH="${TMPDIR:-/tmp}/rnrlottie-verify-npm-package.$$"
mkdir -p "$SCRATCH"
trap 'rm -rf "$SCRATCH"' EXIT

# --- pack a REAL tarball, list its real contents -----------------------------
#
# `npm pack --dry-run --json` is not used: it still executes the `prepare`
# lifecycle script, whose stdout (the "no build step yet" echo) lands on the
# SAME stdout stream ahead of the JSON and corrupts it. Packing to a real
# tarball and listing it with `tar` sidesteps that entirely and is also a more
# faithful check — it is literally the artifact `npm publish` would upload.
echo "== packing =="
PACK_LOG="$SCRATCH/pack.log"
if ! (cd "$ROOT" && npm pack --pack-destination "$SCRATCH" > "$PACK_LOG" 2>&1); then
  echo "npm pack failed:" >&2
  cat "$PACK_LOG" >&2
  exit 2
fi
TARBALL="$(ls "$SCRATCH"/*.tgz 2>/dev/null | head -1)"
if [ -z "$TARBALL" ] || [ ! -f "$TARBALL" ]; then
  echo "npm pack did not produce a .tgz in $SCRATCH" >&2
  cat "$PACK_LOG" >&2
  exit 2
fi

PACKED_BYTES="$(wc -c < "$TARBALL" | tr -d ' ')"
LISTING="$SCRATCH/listing.txt"
# Entries are "package/<path>" — strip the leading "package/".
tar -tzf "$TARBALL" | sed 's#^package/##' > "$LISTING"
N_ENTRIES="$(wc -l < "$LISTING" | tr -d ' ')"

UNPACK_DIR="$SCRATCH/unpacked"
mkdir -p "$UNPACK_DIR"
tar -xzf "$TARBALL" -C "$UNPACK_DIR"
UNPACKED_BYTES="$(du -sk "$UNPACK_DIR/package" | cut -f1)"
UNPACKED_BYTES=$((UNPACKED_BYTES * 1024))

echo "tarball: $TARBALL"
echo "packed size:   $PACKED_BYTES bytes"
echo "unpacked size: $UNPACKED_BYTES bytes ($N_ENTRIES entries)"

# path_in_tarball PATTERN -> true if any packed path matches (grep -E).
path_present() {
  grep -qE "$1" "$LISTING"
}

# ---------------------------------------------------------------------------
# REQUIRED
# ---------------------------------------------------------------------------
echo
echo "== required files =="

require_one() {
  # require_one DESCRIPTION REGEX
  if path_present "$2"; then
    echo "-- present: $1"
  else
    fail "missing required: $1 (pattern: $2)"
  fi
}

require_one "podspec"                          '^react-native-rlottie\.podspec$'
require_one "react-native.config.js"            '^react-native\.config\.js$'
require_one "package.json (implicit, always included by npm)" '^package\.json$'
require_one "THIRD_PARTY_NOTICES.md"            '^THIRD_PARTY_NOTICES\.md$'
require_one "licenses/rlottie-LICENSE.txt"      '^licenses/rlottie-LICENSE\.txt$'

require_one "src/index.ts"                      '^src/index\.ts$'
require_one "src/RlottieView.tsx"               '^src/RlottieView\.tsx$'
require_one "src/types.ts"                      '^src/types\.ts$'

require_one "codegen spec: src/specs/RlottieViewNativeComponent.ts" '^src/specs/RlottieViewNativeComponent\.ts$'
require_one "codegen spec: src/specs/NativeRlottieModule.ts"        '^src/specs/NativeRlottieModule\.ts$'

require_one "android/build.gradle"              '^android/build\.gradle$'
require_one "android/src/main/cpp/CMakeLists.txt" '^android/src/main/cpp/CMakeLists\.txt$'
require_one "android/src/main/cpp/RlottieJni.cpp" '^android/src/main/cpp/RlottieJni\.cpp$'
require_one "android/src/main/cpp/react-native-rlottie.expmap" '^android/src/main/cpp/react-native-rlottie\.expmap$'
require_one "Kotlin: RlottieView.kt"            '^android/src/main/java/com/rlottie/RlottieView\.kt$'
require_one "Kotlin: RlottieBridge.kt"           '^android/src/main/java/com/rlottie/RlottieBridge\.kt$'
require_one "Kotlin: RlottieViewManager.kt"      '^android/src/main/java/com/rlottie/RlottieViewManager\.kt$'
require_one "Kotlin: RlottiePackage.kt"          '^android/src/main/java/com/rlottie/RlottiePackage\.kt$'
require_one "Kotlin: RlottieModule.kt"          '^android/src/main/java/com/rlottie/RlottieModule\.kt$'
require_one "Kotlin: RlottieSourceResolver.kt"  '^android/src/main/java/com/rlottie/RlottieSourceResolver\.kt$'
require_one "Kotlin: RlottieEvents.kt"          '^android/src/main/java/com/rlottie/RlottieEvents\.kt$'

require_one "ios adapter: RNRlottieView.mm"     '^ios/RNRlottieView\.mm$'
require_one "ios adapter: RNRlottiePlayer.mm"   '^ios/RNRlottiePlayer\.mm$'
require_one "ios ViewManager: RNRlottieViewManager.mm" '^ios/RNRlottieViewManager\.mm$'

require_one "shared C++ core: RlottiePlayerCore.cpp"  '^cpp/RlottiePlayerCore\.cpp$'
require_one "shared C++ core: RenderCoordinator.cpp"  '^cpp/RenderCoordinator\.cpp$'
require_one "shared C++ core: FrameBuffer.cpp"        '^cpp/FrameBuffer\.cpp$'
require_one "shared C++ core: InputLimits.h"          '^cpp/InputLimits\.h$'
require_one "shared C++ core: RlottieVersion.h"       '^cpp/RlottieVersion\.h$'

# The vendored rlottie SOURCE, not just the directory: a consumer compiles
# these from source (podspec subspec + CMakeLists.txt add_subdirectory), so
# shipping the directory without its real .cpp/.h payload would install fine
# and fail at the FIRST native build — the worst possible time to find out.
require_one "vendored rlottie: CMakeLists.txt"        '^cpp/third_party/rlottie/CMakeLists\.txt$'
require_one "vendored rlottie: inc/rlottie.h (public header)" '^cpp/third_party/rlottie/inc/rlottie\.h$'
require_one "vendored rlottie: src/lottie/lottieanimation.cpp" '^cpp/third_party/rlottie/src/lottie/lottieanimation\.cpp$'
require_one "vendored rlottie: src/lottie/lottieparser.cpp"    '^cpp/third_party/rlottie/src/lottie/lottieparser\.cpp$'
require_one "vendored rlottie: src/vector/vdrawhelper.cpp"     '^cpp/third_party/rlottie/src/vector/vdrawhelper\.cpp$'
require_one "vendored rlottie: src/vector/freetype/v_ft_raster.cpp" '^cpp/third_party/rlottie/src/vector/freetype/v_ft_raster\.cpp$'
require_one "vendored rlottie: src/vector/stb/stb_image.cpp"   '^cpp/third_party/rlottie/src/vector/stb/stb_image\.cpp$'
require_one "vendored rlottie: src/lottie/rapidjson/document.h" '^cpp/third_party/rlottie/src/lottie/rapidjson/document\.h$'
require_one "vendored rlottie per-component license notices dir" '^cpp/third_party/rlottie/licenses/COPYING\.MIT$'

# A crude but effective floor: real rlottie .cpp payload should be at least
# ~30 files, not 1-2 stragglers left over from a broken exclude pattern.
N_RLOTTIE_CPP="$(grep -cE '^cpp/third_party/rlottie/src/.*\.cpp$' "$LISTING" || true)"
if [ "$N_RLOTTIE_CPP" -lt 20 ]; then
  fail "only $N_RLOTTIE_CPP vendored rlottie .cpp files packed — expected 20+; the vendored tree looks truncated"
else
  echo "-- $N_RLOTTIE_CPP vendored rlottie .cpp files packed"
fi

# ---------------------------------------------------------------------------
# FORBIDDEN
# ---------------------------------------------------------------------------
echo
echo "== forbidden files =="

forbid_none() {
  # forbid_none DESCRIPTION REGEX
  local hits
  hits="$(grep -E "$2" "$LISTING" || true)"
  if [ -n "$hits" ]; then
    fail "forbidden path(s) present ($1):"$'\n'"$hits"
  else
    echo "-- absent: $1"
  fi
}

forbid_none "tests/"                       '(^|/)tests/'
forbid_none "docs/"                        '(^|/)docs/'
forbid_none "build/ (build output dirs)"   '(^|/)build/'
forbid_none ".cxx/ (CMake build cache)"    '(^|/)\.cxx/'
forbid_none ".gradle/"                     '(^|/)\.gradle/'
forbid_none "example/"                     '(^|/)example/'
forbid_none "node_modules/"                '(^|/)node_modules/'
forbid_none "detailed-implementation-plan.md"              '^detailed-implementation-plan\.md$'
forbid_none "react-native-rlottie-implementation-plan.md"  '^react-native-rlottie-implementation-plan\.md$'
forbid_none ".DS_Store"                    '(^|/)\.DS_Store$'
forbid_none "*.o build residue"            '\.o$'
forbid_none "*.so build residue"           '\.so$'
forbid_none "*.a build residue"            '\.a$'
forbid_none "test/spec source files (*.test.ts(x), __tests__/)" '(\.test\.tsx?$|(^|/)__tests__/)'

# ---------------------------------------------------------------------------
# License requirement, not optional
# ---------------------------------------------------------------------------
echo
echo "== license files =="
require_one "licenses/COPYING.FTL (rlottie sub-component notice, vendored copy)" 'cpp/third_party/rlottie/licenses/COPYING\.FTL$'
require_one "licenses/COPYING.PIX (rlottie sub-component notice, vendored copy)" 'cpp/third_party/rlottie/licenses/COPYING\.PIX$'
require_one "licenses/COPYING.STB (rlottie sub-component notice, vendored copy)" 'cpp/third_party/rlottie/licenses/COPYING\.STB$'
require_one "licenses/COPYING.RPD (rlottie sub-component notice, vendored copy)" 'cpp/third_party/rlottie/licenses/COPYING\.RPD$'
require_one "licenses/COPYING.SKIA (rlottie sub-component notice, vendored copy)" 'cpp/third_party/rlottie/licenses/COPYING\.SKIA$'
require_one "licenses/COPYING.MPL (rlottie sub-component notice, vendored copy)" 'cpp/third_party/rlottie/licenses/COPYING\.MPL$'
require_one "licenses/COPYING.MIT (rlottie sub-component notice, vendored copy)" 'cpp/third_party/rlottie/licenses/COPYING\.MIT$'

# ---------------------------------------------------------------------------
# Size sanity
# ---------------------------------------------------------------------------
echo
echo "== size =="
# No hard ceiling asserted (rlottie source is inherently a few MB) — this is a
# visibility gate, not a policy knob: it prints the number so a future
# accidental inclusion (a stray build dir, a committed binary) is obvious in
# CI output rather than silently shipping. 20 MiB is comfortably above today's
# real size (~2.7 MB) and comfortably below "something got bundled in" sizes
# like the 56 MB android/.cxx regression this script was written to catch.
SANITY_CEILING_BYTES=$((20 * 1024 * 1024))
if [ "$UNPACKED_BYTES" -gt "$SANITY_CEILING_BYTES" ]; then
  fail "unpacked size $UNPACKED_BYTES bytes exceeds the $SANITY_CEILING_BYTES byte sanity ceiling — investigate what grew"
else
  echo "-- unpacked size $UNPACKED_BYTES bytes is within the $SANITY_CEILING_BYTES byte sanity ceiling"
fi

echo
if [ "$status" -eq 0 ]; then
  echo "NPM PACKAGE VERIFICATION OK"
else
  echo "NPM PACKAGE VERIFICATION FAILED"
fi
exit "$status"
