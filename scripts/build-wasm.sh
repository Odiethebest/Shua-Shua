#!/usr/bin/env bash
# Build the Shua Shua engine to WebAssembly via Emscripten (Milestone M3).
#
# Produces web/public/shuashua.js — a single-file, classic (non-ESM) module that
# defines a global `ShuaShua()` factory and has the .wasm embedded. The frontend
# loads it with a plain <script> tag (public assets can be referenced via HTML
# tags but not ESM-imported under Vite), and Node loads it by evaluating the file.
#
# Requires emcc on PATH (e.g. `brew install emscripten`). No CMake needed: a
# single emcc invocation mirrors the native single-clang++ build.
#
#   -lembind        : binds the C++ functions in bindings.cpp
#   -sMODULARIZE=1  : expose a factory instead of running on load
#   -sEXPORT_NAME   : the global the factory is attached to
#   -sSINGLE_FILE=1 : embed the .wasm in the .js (nothing else to fetch)
#   -sENVIRONMENT   : run in a browser and in Node (for the smoke test)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

mkdir -p web/public

emcc -std=c++20 -O2 -lembind \
  -sMODULARIZE=1 -sEXPORT_NAME=ShuaShua -sSINGLE_FILE=1 \
  -sENVIRONMENT=web,node \
  src/bindings.cpp -o web/public/shuashua.js

echo "built web/public/shuashua.js (single-file, wasm embedded)"
