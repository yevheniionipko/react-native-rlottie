#!/usr/bin/env bash
#
# Chunk 4.1 — TypeScript-logic tests.
#
#   tests/js/run-tests.sh
#
# Compiles the pure-logic parts of src/ to CommonJS with the tsc that is already
# a devDependency, then runs the tests under plain node. No test framework and no
# new dependencies — the same rule the hand-rolled C++ harness in tests/cpp
# follows.
#
# `react-native` is stubbed (tests/js/stubs) because src/source.ts imports it for
# Image.resolveAssetSource; everything else under test is platform-free.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/tests/js/out"

rm -rf "$OUT"
mkdir -p "$OUT"

echo "== compiling src/ -> $OUT"
# Uses tests/js/tsconfig.json, which EXTENDS the package tsconfig — see the
# comment in that file for why the flags are not spelled out here.
npx --no-install tsc --project "$ROOT/tests/js/tsconfig.json"

# Resolve `require('react-native')` to the stub. A node_modules dir beside the
# compiled output is the least magical way to do it (no loader hooks, no
# NODE_PATH), and it keeps the stub out of the package's own node_modules.
mkdir -p "$OUT/node_modules"
cp "$ROOT/tests/js/stubs/react-native.js" "$OUT/node_modules/react-native.js"

echo "== running tests"
node "$ROOT/tests/js/source.test.js"
node "$ROOT/tests/js/commands.test.js"
node "$ROOT/tests/js/module.test.js"
# view.test.js renders RlottieView through react-test-renderer (Chunk 8.1). It
# needs `react`/`react-test-renderer` resolvable, which they are from the repo's
# own node_modules — and their versions must MATCH: react-test-renderer declares
# a hard peer on the same React minor and crashes on load against a different
# one (this was the reason the render path stayed untested through Phase 7).
node "$ROOT/tests/js/view.test.js"
