#!/usr/bin/env bash
# bench/run_all.sh — the one command. Builds and runs every benchmark, prints an
# environment header first, and exits non-zero if the parity diff fails.
#
#   bash bench/run_all.sh                 # everything available on this machine
#   bash bench/run_all.sh --require-neon  # also fail if the NEON kernel is absent
#   bash bench/run_all.sh --parity-only   # just the gating check (what CI runs)
#
# The output of a full run is what gets pinned into bench/RESULTS.md. Every number
# quoted in README.md or docs/ must come from that file, not from an ad-hoc run.

set -uo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bench="$root/bench"
out="$bench/.build"
mkdir -p "$out"

require_neon=""
parity_only=0
for arg in "$@"; do
  case "$arg" in
    --require-neon) require_neon="--require-neon" ;;
    --parity-only)  parity_only=1 ;;
    *) echo "unknown flag: $arg" >&2; exit 2 ;;
  esac
done

CXX="${CXX:-clang++}"
# An ARRAY, not a string: the repo path may contain spaces, and an unquoted
# expansion of a flags string would split "-I/…/Personal Projects/…" in half.
CXXFLAGS=(-std=c++20 -O2 -I"$root/src" -I"$bench")

hr() { printf '%.0s-' {1..78}; echo; }

# ---------------------------------------------------------------- environment
hr
echo "Shua Shua — benchmark suite"
hr
echo "date            : $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "commit          : $(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo 'n/a') \
$(git -C "$root" diff --quiet 2>/dev/null && echo '(clean)' || echo '(DIRTY — numbers may not match the tree)')"
echo "uname           : $(uname -srm)"
if [ "$(uname -s)" = "Darwin" ]; then
  echo "cpu             : $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
  echo "cores           : $(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || sysctl -n hw.physicalcpu) performance"
  echo "os              : $(sw_vers -productName) $(sw_vers -productVersion)"
elif [ -r /proc/cpuinfo ]; then
  echo "cpu             : $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
  echo "cores           : $(nproc) logical"
fi
echo "compiler        : $($CXX --version 2>/dev/null | head -1)"
echo "cxxflags        : ${CXXFLAGS[*]}"
command -v node >/dev/null 2>&1 && echo "node            : $(node --version)"
command -v emcc >/dev/null 2>&1 && echo "emcc            : $(emcc --version 2>/dev/null | head -1)"
hr

status=0

build_and_run() {
  local name="$1"; shift
  if ! "$CXX" "${CXXFLAGS[@]}" "$bench/$name.cpp" -o "$out/$name" 2>"$out/$name.log"; then
    echo
    echo "=== $name: BUILD FAILED ==="
    sed 's/^/  /' "$out/$name.log"
    status=1
    return
  fi
  "$out/$name" "$@"
  local rc=$?
  if [ $rc -ne 0 ]; then
    echo "  ($name exited $rc)"
    status=$rc
  fi
}

# ------------------------------------------------------------------- parity
# Always first: it is the correctness gate, and everything else is only
# meaningful if the two kernels agree.
build_and_run bench_parity $require_neon

if [ "$parity_only" -eq 1 ]; then
  hr
  echo "parity-only run complete (exit $status)"
  exit $status
fi

# --------------------------------------------------------------- performance
build_and_run bench_dot
build_and_run bench_recall
build_and_run bench_layout
build_and_run bench_pipeline

# ---------------------------------------------------------------------- wasm
if command -v node >/dev/null 2>&1 && [ -f "$root/web/public/shuashua.js" ]; then
  node "$bench/bench_wasm.mjs" || status=$?
else
  echo
  echo "=== wasm — per operator and boundary overhead ==="
  echo "  SKIP: needs node and web/public/shuashua.js"
fi

bash "$bench/probe_wasm_simd.sh" || true

hr
echo "suite complete (exit $status)"
hr
exit $status
