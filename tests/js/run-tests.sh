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

# view.test.js renders RlottieView through react-test-renderer (Chunk 8.1),
# which is the only way to test the component's render-time behaviour.
#
# react-test-renderer reaches into React's internals, so a mismatched pair does
# not merely warn — it throws at REQUIRE time ("Cannot read properties of
# undefined (reading 'S')" for an rtr@19 against react@18). package.json pins a
# matching pair, so a normal `npm install` runs this file. The probe below
# exists for the case where node_modules does NOT match what package.json
# declares (e.g. an install that could not reach the registry): report loudly
# and keep going, rather than failing the whole JS gate for an environment
# problem or — worse — skipping in silence.
if node -e 'require("react-test-renderer")' >/dev/null 2>&1; then
  node "$ROOT/tests/js/view.test.js"
else
  echo
  echo "!! SKIPPING tests/js/view.test.js — react-test-renderer cannot load."
  echo "   Installed: react=$(node -p 'require("react/package.json").version' 2>/dev/null || echo '?')" \
       "react-test-renderer=$(node -p 'require("react-test-renderer/package.json").version' 2>/dev/null || echo '?')"
  echo "   These must be the matching pair package.json declares. Run \`npm install\`."
  echo "   RlottieView's render-time behaviour is NOT covered by this run."
  echo
fi
