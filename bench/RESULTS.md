# bench/RESULTS.md — the pinned official run

This file is a **verbatim capture** of one full `bash bench/run_all.sh` on one
stated machine. It is the single source of numbers for this repository:
**README.md and everything under `docs/` quote this file and nothing else.**

Absolute times are machine-specific. Ratios (speedups, percentage shares) travel
better than microseconds, but even those move a little run to run — the scan
speedup, for instance, sits around 3x here and has been observed between 2.9x and
4.3x depending on which granularity is measured and how warm the machine is.
When quoting, quote the ratio and say which row it came from.

**Re-pinning:** replace this file wholesale with a fresh capture and update every
citation. Do not hand-edit individual numbers — the point of a pinned run is that
the whole table came from one coherent execution.

## How to reproduce

```bash
git checkout 46b9ec3        # or the commit named in the header below
bash bench/run_all.sh
```

Needs a C++20 compiler; `node` and `emcc` are optional (those sections skip
cleanly without them). See `bench/README.md` for what each target measures and
for the three caveats about reading the numbers.

## Caveats that apply to the whole capture

1. **`kernel backend` is printed in every section — read it first.** Only the
   arm64 rows exercise the hand-written NEON kernel. On any other target
   `dot_simd` compiles to `return dot_scalar(...)` (`dot.hpp:86-90`).
2. **Harnesses differ between sections; do not subtract across them.**
   parity/dot/recall/layout use min-of-interleaved-rounds; pipeline and wasm use
   means over the same calls. Each section prints its own `harness:` line.
3. **The deployed browser engine is scalar.** The wasm sections are not a
   measurement of SIMD; see the codegen probe at the end for the proof.

---

## Raw output

```text
------------------------------------------------------------------------------
Shua Shua — benchmark suite
------------------------------------------------------------------------------
date            : 2026-08-02 19:18:09 UTC
commit          : 46b9ec3 (clean)
uname           : Darwin 25.6.0 arm64
cpu             : Apple M4 Pro
cores           : 8 performance
os              : macOS 26.6
compiler        : Apple clang version 21.0.0 (clang-2100.1.1.101)
cxxflags        : -std=c++20 -O2 -I/Users/odieyang/Documents/Projects/Personal Projects/Shua Shua/src -I/Users/odieyang/Documents/Projects/Personal Projects/Shua Shua/bench
node            : v26.5.0
emcc            : emcc (Emscripten gcc/clang-like replacement + linker emulating GNU ld) 6.0.3-git
------------------------------------------------------------------------------

=== parity: naive vs SIMD recall kernel ===
  store            : 3000 items, DIM=64, k=300
  kernel backend   : NEON (arm64 intrinsics)

  -- numerical diff over ALL 3000 items --
  max |naive - simd|                             2.980e-07  (worst item id 1239)
  bit-identical results                          583 / 3000
  tolerance                                      1e-04

  -- ranking diff over top-300 --
  positional differences                         0
  same item set                                  yes

  -- speedup --
  harness: min of 30 rounds x 100 iters, variants interleaved
  dot scan, naive                                    27.96 us
  dot scan, simd                                      9.03 us
  scan speedup                                        3.09x
  end-to-end recall, naive                           50.03 us
  end-to-end recall, simd                            31.80 us
  recall speedup (top-k sort is shared)               1.57x

  VERDICT: PASS

=== dot kernel — three granularities ===
  store          : 3000 items, DIM=64
  kernel backend : NEON (arm64 intrinsics)
  vectorisation  : across DIM (dot.hpp:57 strides d by 4), NOT across items
                   -> one horizontal reduction per item (dot.hpp:74-75)
  work per scan  : 3000 dots x 64 dims = 192000 multiply-adds

  harness: min of 30 rounds x 200 iters, variants interleaved

  [1] one dot (64 floats)
  scalar                                            0.0090 us
  simd                                              0.0025 us
  speedup                                             3.66x

  [2] full scan of 3000 items, no allocation
  scalar                                             27.74 us
  simd                                                6.22 us
  speedup                                             4.46x

  [3] score_all() — scan + build vector<Scored>
  scalar                                             28.05 us
  simd                                                9.38 us
  speedup                                             2.99x

  allocation overhead inside score_all (simd)         3.16 us  (34% of [3])
  note: speedup falls from [1] to [3] because the vector build is not vectorised.

=== recall breakdown — scan vs top-k, and top-k strategies ===
  store          : 3000 items, DIM=64, k=300
  kernel backend : NEON (arm64 intrinsics)

  harness: min of 30 rounds x 100 iters, variants interleaved

  [1] scalar scan only                               28.35 us
  [2] simd scan only                                  8.92 us
  [3] scalar scan + std::sort   (pre-M2)             67.82 us
  [4] simd scan + std::sort     (CURRENT)            48.49 us
  [5] simd scan + partial_sort                       25.78 us
  [6] simd scan + nth_element + sort(k)              18.01 us

  top-k cost inside CURRENT recall  [4]-[2]          39.57 us
    ... as a share of CURRENT recall                 81.6%
  scan speedup from SIMD            [1]/[2]           3.18x
  recall speedup M2 delivered       [3]/[4]           1.40x
  recall speedup nth_element adds   [4]/[6]           2.69x

  nth_element produces the identical top-300 order: yes

=== layout — SoA vs AoS, same SIMD kernel ===
  store          : 3000 items, DIM=64
  kernel backend : NEON (arm64 intrinsics)
  SoA bytes touched per scan  : 768000
  sizeof(ItemAoS)             : 272
  AoS bytes touched per scan  : 816000  (+6.2%)

  harness: 9 independent trials, each min of 20 rounds x 200 iters,
           variants interleaved within every round

  SoA scan, median of trials                          6.83 us
  AoS scan, median of trials                          6.81 us
  SoA advantage, median                               1.7%
  SoA advantage, full spread                        -4.7% .. 4.4%
  trials where SoA was faster                    6 / 9 trials

  note: The spread STRADDLES ZERO: at this store size the layout
  note: difference is NOT RESOLVABLE above run-to-run noise. Do not
  note: quote a percentage for it; quote that it is unmeasurable here.

  note: AoS runs the SAME dot_simd at nearly the same speed, because
  note: `float vec[DIM]` is contiguous there too. Vectorisation needs
  note: contiguous embedding blocks; it does NOT need SoA.
  note: SoA's own benefit is the metadata it keeps out of cache, and at
  note: this store size the whole thing fits in L2, so it stays small.

=== pipeline — end-to-end, and what the trace misses ===
  store          : 3000 items, DIM=64
  kernel backend : NEON (arm64 intrinsics)
  page shape     : 3000 -> 300 -> 50 -> 24 -> 12

  harness: mean of 2000 calls (after 200 warm-up), trace sum and wall
           clock taken from the SAME calls

  -- per operator (as the trace reports) --
  RecallOp    3000 -> 300                            36.06 us
  FeatureOp    300 -> 300                             0.58 us
  ScoreOp      300 -> 50                              1.98 us
  RerankOp      50 -> 24                              1.48 us
  MixOp         24 -> 12                              1.91 us
  SUM of trace latencies                             42.02 us

  -- totals, same calls --
  recommend_from_profile() wall, mean                47.58 us
    (best single call observed)                      39.67 us
    UNACCOUNTED by the trace                          5.56 us
    trace coverage of the call                       88.3%

  -- where the unaccounted time goes (min-of-rounds, separate harness) --
  full_pool()            (api.hpp:140)                3.06 us
  seed copy              (scheduler.hpp:33)           1.14 us
  to_json()              (bindings.cpp:54)            8.70 us
  note: to_json runs AFTER the pipeline (bindings.cpp:54), so it is not part
  note: of the call above at all — it is additional cost the trace never sees.
  full C++ cost per request (call + to_json)         56.28 us
    trace coverage incl. serialisation               74.7%

  seed pool: 3000 candidates x 24 B = 72000 B, built once then copied once
  note: RecallOp ignores that seed (recall_op.hpp:117) — it is a SOURCE node,
  note: so the pool exists to give the uniform in_count contract a funnel mouth.

=== wasm — per operator and boundary overhead ===
  runtime        : Node 26.5.0 (V8 14.6.202.34-node.24)
  engine         : web/public/shuashua.js (207658 chars)
  kernel backend : scalar fallback (emcc defines no __ARM_NEON, no -msimd128)

  COLD first call: trace 834.5us, wall 6414.8us  <- JIT warm-up, do not quote

  harness: warmed with 200 calls, then mean of 500 calls

  -- per operator (as the trace reports) --
  RecallOp 3000 -> 300                               78.19 us
  FeatureOp 300 -> 300                                0.70 us
  ScoreOp 300 -> 50                                   2.55 us
  RerankOp 50 -> 24                                   1.22 us
  MixOp 24 -> 12                                      1.79 us
  SUM of trace latencies                             84.45 us

  -- boundary --
  payload in  (weightsCsv chars)                        13
  payload out (json chars)                            2041
  JS wall clock per call                            110.04 us
  boundary overhead (wall - trace)                   25.59 us
    ... as a share of wall clock                     23.3%

  note: boundary = CSV parse (bindings.cpp:28-53) + to_json (api.hpp:186-221)
        + embind string marshalling both ways + JSON.parse (engine.ts:91).
        The payload is materialised 5 times per request.

=== wasm SIMD codegen probe ===
  emcc           : emcc (Emscripten gcc/clang-like replacement + linker emulating GNU ld) 6.0.3-git

  -- predefined macros --
  emcc (as build-wasm.sh invokes it):
    ARM_NEON=undefined
    WASM_SIMD=undefined
  emcc -msimd128:
    ARM_NEON=undefined
    WASM_SIMD=DEFINED
  => dot.hpp:49 guards on __ARM_NEON, which emcc never defines,
     so dot_simd compiles to 'return dot_scalar(...)' (dot.hpp:86-90).

  -- the SHIPPED engine (web/public/shuashua.js) --
  extracted 175497 bytes -> /var/folders/s_/5rmd3q89665bzhtsrcnlqxkr0000gn/T/tmp.bvF29lVLDl/shipped.wasm
    magic ok, wasm version 1
  shipped sha256 : fde6772a46c6022ff82320a945f625734e1319ca0dd74127230a93b9b61a5b2f
  rebuilt sha256 : fde6772a46c6022ff82320a945f625734e1319ca0dd74127230a93b9b61a5b2f
  => committed artifact is IN SYNC with src/ (byte-identical rebuild)
  SIMD instructions in the shipped wasm : 0
  => the deployed engine runs a PURE SCALAR dot product

  -- dot kernel in isolation (no libc noise) --
  baseline                          4 f32.add 8 f32.load 4 f32.mul 
  with -msimd128                    4 f32.add 8 f32.load 4 f32.mul 
  with -msimd128 -ffast-math        17 f32x4.add 1 f32x4.extract_lane 16 f32x4.mul 32 v128.load 
  => -msimd128 ALONE changes nothing: clang will not vectorise a float
     reduction without -ffast-math, which would invalidate the parity
     premise (see dot.hpp:26-30).

------------------------------------------------------------------------------
suite complete (exit 0)
------------------------------------------------------------------------------
```
