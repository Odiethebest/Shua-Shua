#!/usr/bin/env bash
# Build the Shua Shua engine to WebAssembly via Emscripten (Milestone M3).
#
# Produces web/shuashua.mjs + web/shuashua.wasm — an ES6 module exposing
# recommend() / personaCount() / personaLabel() to JS (browser or Node).
#
# Requires emcc on PATH (e.g. `brew install emscripten`). No CMake needed: a
# single emcc invocation mirrors the native single-clang++ build.
#
#   -lembind          : embind runtime (binds the C++ functions in bindings.cpp)
#   -sMODULARIZE=1     : export a factory function instead of a global Module
#   -sEXPORT_ES6=1     : emit an ES6 module (import createModule from './...mjs')
#   -sENVIRONMENT=web,node : run in both a browser and Node (for the smoke test)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

emcc -std=c++20 -O2 -lembind \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,node \
  src/bindings.cpp -o web/shuashua.mjs

echo "built web/shuashua.mjs + web/shuashua.wasm"
