#!/usr/bin/env bash
#
# Chunk 8.1 — ABI verification.
#
# Builds libreact-native-rlottie.so for every ABI this package ships (read
# from android/build.gradle + android/src/main/cpp/CMakeLists.txt, never
# hardcoded — an ABI list here is exactly the kind of thing that drifts
# silently) and asserts, per ABI:
#
#   1. it links at all                              (extends check-android-build.sh --link)
#   2. every Java_com_rlottie_* JNI entry point is actually EXPORTED
#      (CLAUDE.md/Chunk 3.5: rlottie's own version script once hid all of
#      them — the .so loaded fine and died at the first native call. A
#      link-only check cannot see this; we assert the real dynsym table.)
#   3. no unintended symbol is exported beyond what
#      react-native-rlottie.expmap allows (Java_com_rlottie_* + JNI_OnLoad)
#   4. ELF load segments are 16 KB (0x4000) aligned — required by Android
#      15+/API 35+, and the Chunk 7.4 emulator (sdk_gphone16k_arm64, API 37)
#      is a real 16 KB-page device, so this is not theoretical.
#   5. libc++_shared.so is not duplicated into the AAR.
#
# Then checks the iOS side as far as possible without a full `pod install`
# (see the "iOS" section for exactly what is and is not verified, and why).
#
# Usage:
#   scripts/verify-abis.sh [<api-level>]
#
# Toolchain: $ANDROID_NDK_HOME (else the newest NDK under $ANDROID_HOME/ndk),
# a cmake+ninja under $ANDROID_HOME/cmake, `ruby` for the podspec syntax check.
# Fails loudly (does not skip) if any required tool is missing.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
API="${1:-24}"
status=0

fail() { echo "FAIL: $*" >&2; status=1; }

# macOS ships bash 3.2 (no `mapfile`, no associative arrays) — this whole
# script is written against that floor rather than assuming bash 4+.
read_lines() {
  # Usage: read_lines arrayname < <(some command)
  # Appends one array element per input line to the named array.
  local __arrname="$1" __line
  while IFS= read -r __line; do
    eval "$__arrname+=(\"\$__line\")"
  done
}

array_contains() {
  # Usage: array_contains needle "${haystack[@]}"
  local needle="$1"; shift
  local x
  for x in "$@"; do
    [ "$x" = "$needle" ] && return 0
  done
  return 1
}

# ---------------------------------------------------------------------------
# 0. Resolve the ABI list from source, not from a hardcoded array.
# ---------------------------------------------------------------------------
#
# android/build.gradle's `abiFilters(*safeExtGet("rlottieAbiFilters", [...]))`
# names the ABIs shipped BY DEFAULT. android/src/main/cpp/CMakeLists.txt names
# any ABI that gets special-cased opt-in handling (currently armeabi-v7a, via
# `if(ANDROID_ABI STREQUAL "...")`). The union of both is "every ABI this
# package ships or explicitly supports" — if either file changes, this list
# changes with it.
GRADLE="$ROOT/android/build.gradle"
CMAKE_TXT="$ROOT/android/src/main/cpp/CMakeLists.txt"
[ -f "$GRADLE" ] || { echo "missing $GRADLE" >&2; exit 2; }
[ -f "$CMAKE_TXT" ] || { echo "missing $CMAKE_TXT" >&2; exit 2; }

DEFAULT_ABI_LINE="$(grep -oE 'safeExtGet\("rlottieAbiFilters", *\[[^]]*\]' "$GRADLE" | grep -oE '\[[^]]*\]' || true)"
if [ -z "$DEFAULT_ABI_LINE" ]; then
  echo "could not find rlottieAbiFilters default array in $GRADLE — gradle file shape changed, update this script" >&2
  exit 2
fi
DEFAULT_ABIS=()
read_lines DEFAULT_ABIS < <(grep -oE '"[a-zA-Z0-9_-]+"' <<<"$DEFAULT_ABI_LINE" | tr -d '"')

OPTIN_ABIS=()
read_lines OPTIN_ABIS < <(grep -oE 'ANDROID_ABI STREQUAL "[a-zA-Z0-9_-]+"' "$CMAKE_TXT" | grep -oE '"[a-zA-Z0-9_-]+"' | tr -d '"')

ABIS=()
for a in "${DEFAULT_ABIS[@]}" "${OPTIN_ABIS[@]}"; do
  if ! array_contains "$a" "${ABIS[@]:-}"; then
    ABIS+=("$a")
  fi
done
if [ "${#ABIS[@]}" -eq 0 ]; then
  echo "resolved an empty ABI list — parsing bug, refusing to silently pass" >&2
  exit 2
fi
echo "ABIs to verify (from build.gradle + CMakeLists.txt): ${ABIS[*]}"

# ---------------------------------------------------------------------------
# Toolchain resolution (mirrors scripts/check-android-build.sh).
# ---------------------------------------------------------------------------
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
[ -d "$BIN" ] || { echo "unexpected NDK layout: $BIN missing" >&2; exit 2; }
READELF="$BIN/llvm-readelf"
[ -x "$READELF" ] || { echo "llvm-readelf missing at $READELF" >&2; exit 2; }

SDK_ROOT="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
CMAKE="$(ls -d "$SDK_ROOT"/cmake/*/bin/cmake 2>/dev/null | sort -V | tail -1)"
NINJA="$(ls -d "$SDK_ROOT"/cmake/*/bin/ninja 2>/dev/null | sort -V | tail -1)"
CMAKE="${CMAKE:-$(command -v cmake || true)}"
if [ -z "$CMAKE" ]; then
  echo "no cmake found (looked in \$ANDROID_HOME/cmake and \$PATH)" >&2
  exit 2
fi
echo "NDK: $NDK (api $API), cmake: $CMAKE"

# Allow-list from the real expmap, not a re-typed copy — if the expmap grows a
# new global entry, this script must not need editing to accept it.
EXPMAP="$ROOT/android/src/main/cpp/react-native-rlottie.expmap"
[ -f "$EXPMAP" ] || { echo "missing $EXPMAP" >&2; exit 2; }
ALLOWED_PATTERNS=()
read_lines ALLOWED_PATTERNS < <(sed -n '/^global:/,/^local:/p' "$EXPMAP" | grep -oE '[A-Za-z_][A-Za-z0-9_*]*;' | sed 's/;$//')
if [ "${#ALLOWED_PATTERNS[@]}" -eq 0 ]; then
  echo "could not parse any 'global:' entries out of $EXPMAP — expmap format changed" >&2
  exit 2
fi
echo "expmap allow-list: ${ALLOWED_PATTERNS[*]}"

symbol_allowed() {
  local name="$1" pat
  for pat in "${ALLOWED_PATTERNS[@]}"; do
    # expmap globs are simple prefix* wildcards here (Java_com_rlottie_*).
    case "$name" in
      ${pat}) return 0 ;;
    esac
  done
  return 1
}

BUILD_ROOT="$ROOT/build/verify-abis"
rm -rf "$BUILD_ROOT"
mkdir -p "$BUILD_ROOT"

for ABI in "${ABIS[@]}"; do
  echo
  echo "== $ABI =============================================================="
  BDIR="$BUILD_ROOT/$ABI"

  echo "-- configure + build"
  if ! "$CMAKE" -S "$ROOT/android/src/main/cpp" -B "$BDIR" -G Ninja \
      ${NINJA:+-DCMAKE_MAKE_PROGRAM="$NINJA"} \
      -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" \
      -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release \
      > "$BDIR.cfg.log" 2>&1; then
    fail "$ABI: CMake configure failed (see $BDIR.cfg.log)"
    continue
  fi
  if ! "$CMAKE" --build "$BDIR" -j > "$BDIR.build.log" 2>&1; then
    fail "$ABI: link failed (see $BDIR.build.log)"
    grep -E "error:" "$BDIR.build.log" | head -5
    continue
  fi
  SO="$BDIR/libreact-native-rlottie.so"
  if [ ! -f "$SO" ]; then
    fail "$ABI: build succeeded but $SO does not exist"
    continue
  fi
  echo "   linked: $SO ($(du -h "$SO" | cut -f1))"

  # -- exported JNI entry points --------------------------------------------
  N_JNI="$("$READELF" --dyn-syms "$SO" 2>/dev/null | grep -c "Java_com_rlottie_" || true)"
  if [ "$N_JNI" -eq 0 ]; then
    fail "$ABI: exports NO Java_com_rlottie_* symbols — version script hid the JNI surface"
  else
    echo "-- $N_JNI Java_com_rlottie_* symbols exported"
  fi

  # -- no unintended exported symbols ---------------------------------------
  # A defined, globally-bound, default-visibility entry in --dyn-syms IS an
  # exported symbol (this is exactly what --version-script controls).
  EXPORTED=()
  read_lines EXPORTED < <(
    "$READELF" --dyn-syms "$SO" 2>/dev/null |
      awk '$4=="FUNC" || $4=="OBJECT" || $4=="IFUNC" {
             # columns: Num Value Size Type Bind Vis Ndx Name
             if ($5 ~ /GLOBAL|WEAK/ && $6 == "DEFAULT" && $7 != "UND") print $NF
           }'
  )
  UNEXPECTED=()
  for sym in "${EXPORTED[@]:-}"; do
    [ -z "$sym" ] && continue
    if ! symbol_allowed "$sym"; then
      UNEXPECTED+=("$sym")
    fi
  done
  if [ "${#UNEXPECTED[@]}" -gt 0 ]; then
    fail "$ABI: exports symbols outside the expmap allow-list: ${UNEXPECTED[*]}"
  else
    echo "-- no unintended exported symbols (${#EXPORTED[@]} exported, all allow-listed)"
  fi

  # -- 16 KB page alignment of LOAD segments --------------------------------
  # Android's 16 KB page-size requirement (API 35+) applies only to 64-bit
  # native libraries — 32-bit libraries run under a compatibility path and are
  # explicitly exempted by Google's own guidance, so armeabi-v7a/x86 are not
  # checked here (and, empirically, the NDK does not 16 KB-align them: r28
  # emits 0x1000-aligned LOAD segments for armeabi-v7a with no way to change
  # that from our side). Read the program headers directly rather than
  # trusting a single "looks fine" heuristic.
  case "$ABI" in
    arm64-v8a|x86_64)
      LOAD_ALIGNS=()
      read_lines LOAD_ALIGNS < <(
        "$READELF" -lW "$SO" 2>/dev/null |
          awk '/^  LOAD/ { print $NF }'
      )
      if [ "${#LOAD_ALIGNS[@]}" -eq 0 ]; then
        fail "$ABI: could not find any LOAD segments in program headers"
      else
        BAD_ALIGN=()
        for a in "${LOAD_ALIGNS[@]}"; do
          # Alignment is printed as hex, e.g. 0x4000, 0x1000.
          dec=$((a))
          if [ "$((dec % 16384))" -ne 0 ]; then
            BAD_ALIGN+=("$a")
          fi
        done
        if [ "${#BAD_ALIGN[@]}" -gt 0 ]; then
          fail "$ABI: LOAD segment alignment ${BAD_ALIGN[*]} is not a multiple of 16384 (16 KB) — will fail to load on API 35+ 16 KB-page devices"
        else
          echo "-- LOAD segments 16 KB aligned (${LOAD_ALIGNS[*]})"
        fi
      fi
      ;;
    *)
      echo "-- skipping 16 KB alignment check for $ABI (32-bit; Android's requirement is 64-bit-only)"
      ;;
  esac

  # -- dynamically links libc++_shared, does not embed it -------------------
  if "$READELF" -d "$SO" 2>/dev/null | grep -q "libc++_shared.so"; then
    echo "-- dynamically depends on libc++_shared.so (expected with ANDROID_STL=c++_shared)"
  else
    fail "$ABI: does not show libc++_shared.so as NEEDED — ANDROID_STL setting may have changed"
  fi
done

# ---------------------------------------------------------------------------
# libc++_shared.so not duplicated into the AAR.
# ---------------------------------------------------------------------------
#
# The definitive check is a real Gradle `assembleRelease` + unzip of the AAR,
# but that needs a Gradle wrapper/distribution this environment does not have
# committed (android/build.gradle is designed to be consumed by a HOST app's
# Gradle, not built standalone without one — see its own header comment).
# What we CAN verify without Gradle:
#   (a) android/build.gradle's packagingOptions actually excludes it (static
#       config check — this is the real mechanism that prevents duplication).
#   (b) if a previously-built AAR is sitting in android/build/outputs/aar/
#       (e.g. left over from a prior manual verification), inspect the real
#       artifact instead of trusting the config.
echo
echo "== libc++_shared.so duplication ======================================"
if grep -q 'excludes += \["\*\*/libc++_shared.so"\]' "$GRADLE"; then
  echo "-- android/build.gradle packagingOptions excludes libc++_shared.so"
else
  fail "android/build.gradle no longer excludes libc++_shared.so from packaging"
fi

EXISTING_AARS=("$ROOT"/android/build/outputs/aar/*.aar)
if [ -e "${EXISTING_AARS[0]}" ]; then
  for AAR in "${EXISTING_AARS[@]}"; do
    N="$(unzip -l "$AAR" | grep -c "libc++_shared.so" || true)"
    if [ "$N" -gt 0 ]; then
      fail "$(basename "$AAR") bundles $N copy/copies of libc++_shared.so"
    else
      echo "-- $(basename "$AAR"): no libc++_shared.so present (real artifact check)"
    fi
  done
else
  echo "-- no pre-built AAR found under android/build/outputs/aar/; only the static"
  echo "   packagingOptions check above ran. A full 'gradlew assembleRelease' inside"
  echo "   a real consumer app is the authoritative check for this item."
fi

# ---------------------------------------------------------------------------
# iOS: as far as possible without a full `pod install` / `pod lib lint`.
# ---------------------------------------------------------------------------
#
# `pod lib lint` needs network access (fetches React-Core et al.) and does a
# full build; that's disproportionate here. Instead:
#   1. `ruby -c` the podspec — catches any syntax error a lint would also hit.
#   2. Actually `Pod::Spec.new` it (via `pod ipc spec`, no network) to catch
#      spec-construction errors (missing method, bad DSL usage) beyond mere
#      Ruby syntax.
#   3. Assert every glob in `source_files`/`preserve_paths` actually matches
#      real files.
#   4. Assert the Apple-arm64 `-U__ARM_NEON__` workaround (CLAUDE.md: the NDK
#      arm64-v8a note's twin — Apple clang arm64 also defines __ARM_NEON__)
#      and the C++14/-fno-exceptions/-fno-rtti flags are actually present.
echo
echo "== iOS podspec ========================================================"
PODSPEC="$ROOT/react-native-rlottie.podspec"
[ -f "$PODSPEC" ] || { echo "missing $PODSPEC" >&2; exit 2; }

if ! command -v ruby >/dev/null 2>&1; then
  echo "ruby not found — cannot check the podspec at all" >&2
  exit 2
fi

if ruby -c "$PODSPEC" > /dev/null; then
  echo "-- ruby -c: syntax OK"
else
  fail "podspec has a Ruby syntax error"
fi

if command -v pod >/dev/null 2>&1; then
  if (cd "$ROOT" && pod ipc spec "$PODSPEC" > /dev/null 2>"$BUILD_ROOT/pod-ipc-spec.log"); then
    echo "-- pod ipc spec: parses as a valid Pod::Spec (no network needed)"
  else
    fail "pod ipc spec could not parse the podspec (see $BUILD_ROOT/pod-ipc-spec.log)"
  fi
else
  echo "-- 'pod' CLI not found; skipping 'pod ipc spec' parse (ruby -c above already checked syntax)"
fi

if grep -q -- '-U__ARM_NEON__' "$PODSPEC"; then
  echo "-- -U__ARM_NEON__ Apple-arm64 workaround present"
else
  fail "podspec is missing the -U__ARM_NEON__ Apple-arm64 NEON workaround (see CLAUDE.md Chunk 3.5 note / CMakeLists.txt armeabi-v7a twin)"
fi
for flag in "std=gnu++14" "fno-exceptions" "fno-rtti"; do
  if grep -q -- "$flag" "$PODSPEC"; then
    echo "-- $flag present"
  else
    fail "podspec is missing expected compiler flag: $flag"
  fi
done

# Resolve every `s.source_files` / `ss.source_files` / `ss.preserve_paths` glob
# against the real tree.
# Two things bash 3.2 (macOS's system bash — no `globstar`) cannot do out of
# the box: expand a `{cpp,h}` brace group, or match `**` recursively. Both
# appear in these podspec globs (CocoaPods uses Ruby's Dir.glob, which
# supports both), so expand/resolve them by hand instead of trusting compgen.
expand_braces() {
  # Single, non-nested {a,b,c} group -> one line per alternative (or the
  # input unchanged if it has no brace group).
  local s="$1"
  if [[ "$s" == *"{"*"}"* ]]; then
    local pre="${s%%\{*}"
    local rest="${s#*\{}"
    local inner="${rest%%\}*}"
    local post="${rest#*\}}"
    local alt
    local oldIFS="$IFS"
    IFS=','
    for alt in $inner; do
      echo "${pre}${alt}${post}"
    done
    IFS="$oldIFS"
  else
    echo "$s"
  fi
}

glob_exists() {
  # A pattern with `**/` is checked as: everything before `**/` is a
  # directory that must exist, and `find -name` (which matches basenames at
  # any depth by default) checks the tail pattern recursively. Anything else
  # is a plain, single-level glob.
  local pattern="$1" dir name
  if [[ "$pattern" == *'**/'* ]]; then
    dir="${pattern%%\*\*/*}"
    name="${pattern#*\*\*/}"
    dir="${ROOT}/${dir%/}"
    [ -d "$dir" ] || return 1
    [ -n "$(find "$dir" -type f -name "$name" -print -quit 2>/dev/null)" ]
  else
    compgen -G "$ROOT/$pattern" > /dev/null
  fi
}

GLOB_LINES=()
read_lines GLOB_LINES < <(grep -oE '"(cpp|ios)/[^"]*"' "$PODSPEC")
if [ "${#GLOB_LINES[@]}" -eq 0 ]; then
  fail "could not find any source_files/preserve_paths globs in the podspec to verify"
else
  MISSING_GLOBS=()
  for g in "${GLOB_LINES[@]}"; do
    glob="${g//\"/}"
    # A brace group like {h,hpp,cpp} legitimately has extensions with zero
    # matches (there are no .hpp files here) — that's normal, not a defect.
    # Only flag the ORIGINAL glob line if NONE of its brace alternatives
    # match anything at all.
    any_match=0
    while IFS= read -r concrete; do
      [ -z "$concrete" ] && continue
      if glob_exists "$concrete"; then
        any_match=1
      fi
    done < <(expand_braces "$glob")
    if [ "$any_match" -eq 0 ]; then
      MISSING_GLOBS+=("$glob")
    fi
  done
  if [ "${#MISSING_GLOBS[@]}" -gt 0 ]; then
    fail "podspec globs match nothing on disk: ${MISSING_GLOBS[*]}"
  else
    echo "-- all ${#GLOB_LINES[@]} source_files/exclude_files/preserve_paths globs match at least one real file"
  fi
fi

echo
if [ "$status" -eq 0 ]; then
  echo "ABI VERIFICATION OK"
else
  echo "ABI VERIFICATION FAILED"
fi
exit "$status"
