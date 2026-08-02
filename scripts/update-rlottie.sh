#!/usr/bin/env bash
#
# update-rlottie.sh — reproducibly vendor a pinned rlottie revision into
# cpp/third_party/rlottie/ (Chunk 0.2).
#
# The npm package ships vendored rlottie SOURCE — never fetched at build time
# (plan §13). Git submodules are unsuitable as the sole delivery mechanism
# because npm consumers may not receive initialized submodules.
#
# What this script does:
#   1. Fetches rlottie at RLOTTIE_COMMIT (a full 40-char SHA) into a temp dir.
#   2. Copies ONLY the build-necessary subset into cpp/third_party/rlottie/,
#      stripping examples, tests, tooling, meson/vs/packaging, CI, arm/wasm
#      build scripts (plan §14: upstream examples/tests must not become app
#      targets).
#   3. Applies a minimal, recorded patch making the example subdir opt-in
#      (upstream's add_subdirectory(example) is UNCONDITIONAL, so stripping it
#      would otherwise break CMake configure).
#   4. Regenerates cpp/RlottieVersion.h, licenses/rlottie-LICENSE.txt, and the
#      rlottie section of THIRD_PARTY_NOTICES.md from the pinned SHA.
#
# Usage:
#   scripts/update-rlottie.sh [<commit-sha>]
# If no SHA is given, RLOTTIE_COMMIT below is used.
#
# SECURITY (plan §13/§16): the pinned SHA MUST carry all relevant rlottie
# memory-safety / input-validation fixes. Verify against the upstream history
# before bumping. Run the native fuzz + malformed-file tests (Chunk 5.5) after
# any update.

set -euo pipefail

RLOTTIE_REPO="${RLOTTIE_REPO:-https://github.com/Samsung/rlottie.git}"
# Pinned rlottie revision. Bump only via this script + re-run the fuzz suite.
RLOTTIE_COMMIT="${1:-2365f5671b67791fc179818fd11b180d79aec612}"

# Resolve repo paths relative to this script (scripts/ is at the repo root).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEST="$ROOT/cpp/third_party/rlottie"
LICENSE_OUT="$ROOT/licenses/rlottie-LICENSE.txt"
VERSION_H="$ROOT/cpp/RlottieVersion.h"
NOTICES="$ROOT/THIRD_PARTY_NOTICES.md"

# Only these paths are vendored. Everything else upstream is dropped.
KEEP=(
  CMakeLists.txt
  cmake
  inc
  src
  licenses
  COPYING
  AUTHORS
  README.md
  rlottie.expmap
  rlottie.pc.in
)

echo "[update-rlottie] repo   : $RLOTTIE_REPO"
echo "[update-rlottie] commit : $RLOTTIE_COMMIT"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "[update-rlottie] fetching…"
git -C "$TMP" init -q
git -C "$TMP" remote add origin "$RLOTTIE_REPO"
if ! git -C "$TMP" fetch -q --depth 1 origin "$RLOTTIE_COMMIT"; then
  echo "[update-rlottie] shallow fetch of SHA failed; falling back to full fetch"
  git -C "$TMP" fetch -q origin
fi
git -C "$TMP" checkout -q "$RLOTTIE_COMMIT"

# Confirm the checked-out SHA matches exactly (reproducibility guard).
GOT="$(git -C "$TMP" rev-parse HEAD)"
if [ "$GOT" != "$RLOTTIE_COMMIT" ]; then
  echo "[update-rlottie] ERROR: checked out $GOT, expected $RLOTTIE_COMMIT" >&2
  exit 1
fi

echo "[update-rlottie] vendoring subset -> $DEST"
rm -rf "$DEST"
mkdir -p "$DEST"
for path in "${KEEP[@]}"; do
  if [ -e "$TMP/$path" ]; then
    cp -R "$TMP/$path" "$DEST/"
  else
    echo "[update-rlottie] WARN: expected path missing upstream: $path" >&2
  fi
done

# --- Recorded patch: make the example subdir opt-in (default OFF). -----------
# Upstream has an UNCONDITIONAL `add_subdirectory(example)`. We strip example/,
# so guard it behind LOTTIE_EXAMPLE (OFF) to keep CMake configure working.
CML="$DEST/CMakeLists.txt"
if grep -qE '^[[:space:]]*add_subdirectory\(example\)' "$CML"; then
  # Insert the option next to the other LOTTIE_* options.
  perl -0pi -e 's/(option\(LOTTIE_TEST [^\n]*\n)/$1option(LOTTIE_EXAMPLE "Build LOTTIE examples (stripped in this vendored copy)" OFF)\n/' "$CML"
  # Guard the add_subdirectory call.
  perl -0pi -e 's/^([[:space:]]*)add_subdirectory\(example\)/$1if (LOTTIE_EXAMPLE)\n$1    add_subdirectory(example)\n$1endif()/m' "$CML"
  echo "[update-rlottie] patched CMakeLists.txt: example subdir is now opt-in (LOTTIE_EXAMPLE=OFF)"
else
  echo "[update-rlottie] NOTE: no unconditional add_subdirectory(example) found; upstream layout changed — review the patch." >&2
fi

# --- Licenses --------------------------------------------------------------
# rlottie's COPYING explains the composite licensing; COPYING.MIT is the
# primary MIT text. Emit a single combined notice file.
{
  echo "rlottie — vendored at commit $RLOTTIE_COMMIT"
  echo "Source: $RLOTTIE_REPO"
  echo
  echo "======================================================================"
  echo "COPYING (licensing overview)"
  echo "======================================================================"
  cat "$TMP/COPYING"
  echo
  echo "======================================================================"
  echo "licenses/COPYING.MIT (primary license)"
  echo "======================================================================"
  cat "$TMP/licenses/COPYING.MIT"
} > "$LICENSE_OUT"
echo "[update-rlottie] wrote $LICENSE_OUT"

# --- Version header --------------------------------------------------------
cat > "$VERSION_H" <<EOF
// Generated by scripts/update-rlottie.sh — do not edit by hand.
// Identifies the vendored rlottie revision (Chunk 0.2). Consumed by the
// deterministic cache key (Chunk 5.3) and getNativeVersion() (Chunks 2.4/3.4).
#pragma once

namespace rnrlottie {

// Full commit SHA of the vendored rlottie source in cpp/third_party/rlottie/.
inline constexpr char kRlottieCommit[] = "$RLOTTIE_COMMIT";

}  // namespace rnrlottie
EOF
echo "[update-rlottie] wrote $VERSION_H"

# --- Third-party notices ---------------------------------------------------
#
# Two parts: an AUTO part (this commit's SHA and the pointers derived from it)
# and a STATIC part (the composite-licensing breakdown of rlottie's own
# vendored tree — which folder maps to which license, and what's actually
# compiled into the binary). The static part does not change on a routine
# re-vendor: it describes rlottie's directory *structure*, not its content at
# a given commit. It only needs hand review if rlottie's own layout changes
# (see the "Notes for maintainers" section it ends with). Both parts are
# rewritten by every run, so the file as a whole is still fully regenerated —
# "regenerated by this script" stays literally true for its entire content,
# not just the commit-specific header.
cat > "$NOTICES" <<EOF
# Third-party notices

\`react-native-rlottie\` bundles third-party source. This file records each
vendored dependency, its license, and the exact pinned revision. The rlottie
section (commit, top-level license pointer) is regenerated by
\`scripts/update-rlottie.sh\`; the sub-component enumeration below it documents
the composite licensing inside that single vendored tree and is maintained by
hand alongside that script (see the note at the end of this file).

---

## rlottie

- **Upstream:** https://github.com/Samsung/rlottie
- **License:** MIT-style (composite; see \`licenses/rlottie-LICENSE.txt\`).
- **Pinned commit:** \`$RLOTTIE_COMMIT\`
- **Vendored at:** \`cpp/third_party/rlottie/\`
- **Version constant:** \`cpp/RlottieVersion.h\` (\`rnrlottie::kRlottieCommit\`)
- **Update procedure:** \`scripts/update-rlottie.sh [<sha>]\`

The vendored copy omits upstream \`example/\`, \`test/\`, tooling, meson/VS/
packaging, CI, and arm/wasm build scripts, and makes the example subdir opt-in
(\`LOTTIE_EXAMPLE=OFF\`) so it never becomes an application target (plan §14).

rlottie's MIT-style license requires the copyright and permission notice to
remain included; the full notice is reproduced in \`licenses/rlottie-LICENSE.txt\`
and the per-component notices are preserved under
\`cpp/third_party/rlottie/licenses/\` (also shipped in the npm package, since
\`cpp/\` is part of \`package.json\`'s \`files\`).

### Sub-components (composite licensing)

rlottie's own \`COPYING\` file documents that "some parts of shared code are
covered by different licenses" than its primary MIT license, and lists a
folder-to-license mapping. The table below re-derives that mapping against
what is **actually compiled into this package's shipped binary** — verified by
reading \`CMakeLists.txt\` at every level (\`src/vector/CMakeLists.txt\`,
\`src/vector/{freetype,pixman,stb}/CMakeLists.txt\`, \`src/lottie/CMakeLists.txt\`)
and the \`#include\`/copyright-header content of the files those lists name,
not just rlottie's own summary.

| Component | License | Notice file (in the package) | Vendored path | Compiled into the shipped binary? |
|---|---|---|---|---|
| rlottie core (own code) | MIT | \`licenses/rlottie-LICENSE.txt\`, \`cpp/third_party/rlottie/licenses/COPYING.MIT\` | \`cpp/third_party/rlottie/src/lottie/*.cpp\` (excluding rapidjson), \`src/vector/*.cpp\` (excluding the sub-components below) | Yes — this is the library. |
| Skia-derived arena allocator | BSD-style ("Skia" license) | \`cpp/third_party/rlottie/licenses/COPYING.SKIA\` | \`src/vector/varenaalloc.{h,cpp}\` (copyright header: \`Copyright (c) 2011 Google Inc.\`) | **Yes.** Compiled unconditionally (\`src/vector/CMakeLists.txt\`). |
| FreeType (rasterizer, adapted) | FTL (FreeType Project LICENSE) | \`cpp/third_party/rlottie/licenses/COPYING.FTL\` | \`src/vector/freetype/{v_ft_math,v_ft_raster,v_ft_stroker}.cpp\` (+ headers) | **Yes.** Compiled unconditionally (\`src/vector/freetype/CMakeLists.txt\`). |
| stb_image | MIT | \`cpp/third_party/rlottie/licenses/COPYING.STB\` | \`src/vector/stb/stb_image.{h,cpp}\` | **Yes**, when \`LOTTIE_MODULE=OFF\` — which is exactly how this package builds it (\`android/src/main/cpp/CMakeLists.txt\` sets \`LOTTIE_MODULE OFF\`; the podspec has no dynamic-loader plugin path either). If a downstream build ever flips \`LOTTIE_MODULE\` on, stb_image instead builds as a separate \`rlottie-image-loader\` shared object — still under the same license, so this notice does not need to change either way. |
| rapidjson (JSON parsing) | MIT (+ third-party sub-notices for \`msinttypes\`/BSD and the JSON.org "Good, not Evil" clause, both reproduced in the same file) | \`cpp/third_party/rlottie/licenses/COPYING.RPD\` | \`src/lottie/rapidjson/**\` | **Yes** — header-only, pulled in via \`#include\` in \`src/lottie/lottieparser.cpp\`, which IS compiled. |
| vinterpolator (cubic-bezier easing) | **MPL 2.0** | \`cpp/third_party/rlottie/licenses/COPYING.MPL\` | \`src/vector/vinterpolator.cpp\` (its own file header: *"This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0..."*) | **Yes.** Compiled unconditionally (\`src/vector/CMakeLists.txt\`). See the finding below — rlottie's own \`COPYING\` file does not list this. |
| pixman (ARM NEON assembly composite ops) | MIT (\`pixman-arm-neon-asm.{h,S}\`, Nokia copyright header) | \`cpp/third_party/rlottie/licenses/COPYING.PIX\` | \`src/vector/pixman/pixman-arm-neon-asm.{h,S}\` | **No, on any ABI this package ships.** See the finding below. |

### Findings from this audit (Chunk 8.1)

1. **\`vinterpolator.cpp\` is MPL-2.0-licensed and IS compiled into the shipped
   binary, but rlottie's own \`COPYING\` mapping omits it.** rlottie's \`COPYING\`
   lists \`src/vector/ -> COPYING.MIT, COPYING.SKIA\` and nothing else for that
   directory, but \`src/vector/vinterpolator.cpp\` (unconditionally compiled,
   see \`src/vector/CMakeLists.txt\` line \`vinterpolator.cpp\`) carries an
   explicit MPL 2.0 file header, distinct from the MIT header on its own
   \`vinterpolator.h\`. \`cpp/third_party/rlottie/licenses/COPYING.MPL\` is
   vendored — so the notice text is present in the package — but nothing in
   rlottie's own documentation, nor (until this file) ours, said which file it
   actually covers. This is a genuine compliance gap this chunk closes: the
   table above is the first place that ties \`COPYING.MPL\` to
   \`vinterpolator.cpp\` explicitly. Treat rlottie's upstream \`COPYING\` file as
   incomplete, not authoritative, for this one file.

2. **\`pixman-arm-neon-asm.{h,S}\` (the only files under \`src/vector/pixman/\`)
   are vendored source but are not compiled into any binary this package
   ships, on either platform.** \`src/vector/pixman/CMakeLists.txt\` only adds
   \`pixman-arm-neon-asm.S\` to the build \`IF("\${ARCH}" STREQUAL "arm")\` — and
   nothing in this package's Android CMake invocation or the iOS podspec ever
   sets a CMake \`ARCH\` variable, so that condition is never true on any
   Android ABI. On iOS the podspec's \`exclude_files\` explicitly drops
   \`**/*.S\`. The \`#if defined(__ARM_NEON__)\` guard in
   \`src/vector/vdrawhelper_neon.cpp\` (which calls into the pixman NEON
   symbols) is also never true on the ABIs this package actually ships: NDK
   Clang only defines the legacy 32-bit macro \`__ARM_NEON__\` on \`armeabi-v7a\`,
   and \`android/src/main/cpp/CMakeLists.txt\` explicitly \`-U__ARM_NEON__\`s it
   there (see that file's comment on the armeabi-v7a NEON trap); \`arm64-v8a\`
   never defines \`__ARM_NEON__\` (only \`__ARM_NEON\`, no trailing underscore) in
   the first place. **Net effect: \`COPYING.PIX\`-covered code is present as
   vendored source in the npm package (a consumer builds from source, so it
   still has to ship) but contributes no code to any binary this package
   produces on any ABI/platform.** Documented here rather than silently
   dropping the notice, since the npm tarball does still distribute the
   source text itself.

---

## Notes for maintainers

- \`scripts/update-rlottie.sh\` regenerates the "rlottie" section above (commit
  SHA, \`cpp/RlottieVersion.h\`, \`licenses/rlottie-LICENSE.txt\`) automatically
  on every re-vendor. The "Sub-components" table and "Findings" section are
  **not** auto-generated — they describe the vendored tree's *structure*
  (which folder maps to which license, and what's actually compiled), which a
  version bump does not change unless rlottie's own directory layout changes.
  If a \`scripts/update-rlottie.sh\` run ever reports a changed vendored layout
  (new/removed top-level dirs in \`KEEP\`, or a changed \`src/vector\`/\`src/lottie\`
  substructure), re-verify this table by hand — do not assume it still holds.
EOF
echo "[update-rlottie] wrote $NOTICES"

echo "[update-rlottie] done. Vendored tree size:"
du -sh "$DEST"
