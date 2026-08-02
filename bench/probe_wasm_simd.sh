#!/usr/bin/env bash
# bench/probe_wasm_simd.sh — does the SHIPPED wasm contain any SIMD instructions?
#
# This is a codegen probe, not a timing benchmark. It answers a question the
# README used to get wrong: whether the deployed engine runs the hand-written NEON
# kernel (it does not) and whether simply adding -msimd128 would fix that (it
# would not).
#
# Three checks:
#   1. Predefined macros under emcc, with and without -msimd128.
#   2. The COMMITTED web/public/shuashua.js: extract the embedded wasm, verify it
#      matches a fresh build byte-for-byte, and count v128 instructions in it.
#   3. An isolated TU containing only the dot kernel, built three ways, so the
#      instruction counts cannot be polluted by SIMD variants of libc memcpy.
#
# Requires emcc and node. Exits 0 and prints SKIP if emcc is unavailable.

set -uo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo
echo "=== wasm SIMD codegen probe ==="

if ! command -v emcc >/dev/null 2>&1; then
  echo "  SKIP: emcc not on PATH (this probe needs Emscripten)"
  exit 0
fi
echo "  emcc           : $(emcc --version 2>/dev/null | head -1)"

# Binaryen ships with emscripten; wasm-dis is how we count instructions.
disasm=""
for cand in \
  "$(command -v wasm-dis 2>/dev/null || true)" \
  "$(dirname "$(command -v emcc)")/../libexec/binaryen/bin/wasm-dis" \
  "/opt/homebrew/opt/emscripten/libexec/binaryen/bin/wasm-dis"; do
  if [ -n "$cand" ] && [ -x "$cand" ]; then disasm="$cand"; break; fi
done

# --- 1. predefined macros ----------------------------------------------------
probe_c="$work/probe.c"
printf '#ifdef __ARM_NEON\nARM_NEON=DEFINED\n#else\nARM_NEON=undefined\n#endif\n#ifdef __wasm_simd128__\nWASM_SIMD=DEFINED\n#else\nWASM_SIMD=undefined\n#endif\n' > "$probe_c"
echo
echo "  -- predefined macros --"
echo "  emcc (as build-wasm.sh invokes it):"
emcc -E -P -x c "$probe_c" 2>/dev/null | sed 's/^/    /'
echo "  emcc -msimd128:"
emcc -msimd128 -E -P -x c "$probe_c" 2>/dev/null | sed 's/^/    /'
echo "  => dot.hpp:49 guards on __ARM_NEON, which emcc never defines,"
echo "     so dot_simd compiles to 'return dot_scalar(...)' (dot.hpp:86-90)."

# --- 2. the committed artifact ------------------------------------------------
echo
echo "  -- the SHIPPED engine (web/public/shuashua.js) --"
node "$root/bench/extract_wasm.mjs" "$root/web/public/shuashua.js" "$work/shipped.wasm" \
  | sed 's/^/  /'

emcc -std=c++20 -O2 -lembind -sMODULARIZE=1 -sEXPORT_NAME=ShuaShua -sENVIRONMENT=web,node \
  "$root/src/bindings.cpp" -o "$work/fresh.js" 2>/dev/null
if [ -f "$work/fresh.wasm" ]; then
  a=$(shasum -a 256 "$work/shipped.wasm" 2>/dev/null | awk '{print $1}')
  b=$(shasum -a 256 "$work/fresh.wasm"   2>/dev/null | awk '{print $1}')
  echo "  shipped sha256 : $a"
  echo "  rebuilt sha256 : $b"
  if [ "$a" = "$b" ]; then
    echo "  => committed artifact is IN SYNC with src/ (byte-identical rebuild)"
  else
    echo "  => WARNING: committed artifact DIFFERS from a fresh build of src/"
  fi
fi

if [ -n "$disasm" ]; then
  n=$("$disasm" "$work/shipped.wasm" 2>/dev/null | grep -cE 'v128|f32x4|i32x4|i8x16|i16x8|f64x2|i64x2' || true)
  echo "  SIMD instructions in the shipped wasm : $n"
  [ "$n" = "0" ] && echo "  => the deployed engine runs a PURE SCALAR dot product"
else
  echo "  (wasm-dis not found; skipped instruction count)"
fi

# --- 3. isolated dot kernel, three builds ------------------------------------
echo
echo "  -- dot kernel in isolation (no libc noise) --"
cat > "$work/dotprobe.cpp" <<'EOF'
#include "dot.hpp"
extern "C" float probe_scalar(const float* a, const float* b) { return dot_scalar(a, b, 64); }
extern "C" float probe_simd(const float* a, const float* b)   { return dot_simd(a, b, 64); }
EOF

if [ -n "$disasm" ]; then
  for spec in "baseline:" "with -msimd128:-msimd128" "with -msimd128 -ffast-math:-msimd128 -ffast-math"; do
    label="${spec%%:*}"; flags="${spec#*:}"
    # shellcheck disable=SC2086
    emcc -std=c++20 -O2 $flags -I"$root/src" -sSTANDALONE_WASM \
      -sEXPORTED_FUNCTIONS=_probe_scalar,_probe_simd --no-entry \
      "$work/dotprobe.cpp" -o "$work/k.wasm" 2>/dev/null
    ops=$("$disasm" "$work/k.wasm" 2>/dev/null \
          | grep -oE 'f32\.load|f32\.mul|f32\.add|v128\.load|f32x4\.[a-z_]+' \
          | sort | uniq -c | tr '\n' ' ' | tr -s ' ')
    printf '  %-32s %s\n' "$label" "$ops"
  done
  echo "  => -msimd128 ALONE changes nothing: clang will not vectorise a float"
  echo "     reduction without -ffast-math, which would invalidate the parity"
  echo "     premise (see dot.hpp:26-30)."
else
  echo "  (wasm-dis not found; skipped)"
fi
echo
