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
cat > "$NOTICES" <<EOF
# Third-party notices

\`react-native-rlottie\` bundles third-party source. This file records each
vendored dependency, its license, and the exact pinned revision. It is
regenerated by \`scripts/update-rlottie.sh\`.

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
\`cpp/third_party/rlottie/licenses/\`.
EOF
echo "[update-rlottie] wrote $NOTICES"

echo "[update-rlottie] done. Vendored tree size:"
du -sh "$DEST"
