# bench/

Measurement harness for the Shua Shua engine. **Not engine code** — nothing here
contains ranking logic; these targets only time and diff what `src/` already does.

```bash
bash bench/run_all.sh                 # everything available on this machine
bash bench/run_all.sh --parity-only   # just the correctness gate (what CI runs)
bash bench/run_all.sh --require-neon  # additionally fail if the NEON path is absent
```

Exit code is non-zero if the parity diff fails, so `run_all.sh --parity-only` is
usable as a CI gate. See `.github/workflows/parity.yml`.

## Targets

| Target | Answers |
|---|---|
| `bench_parity.cpp` | Do `dot_scalar` and `dot_simd` produce the same numbers? **Gating.** |
| `bench_dot.cpp` | What is the kernel speedup, at three granularities? |
| `bench_recall.cpp` | Inside `RecallOp`, is the scan or the top-k the bottleneck? |
| `bench_layout.cpp` | What does SoA buy, *separately* from what SIMD needs? |
| `bench_pipeline.cpp` | What fraction of a request does the pipeline trace account for? |
| `bench_wasm.mjs` | What does the **browser** pay, including the JS↔WASM boundary? |
| `probe_wasm_simd.sh` | Does the shipped `.wasm` contain any SIMD instructions? |

`extract_wasm.mjs` is a helper: it pulls the embedded `.wasm` out of the
`-sSINGLE_FILE` build so the probe can disassemble it.

## The parity check exists twice, on purpose

`src/main.cpp:66-147` (`run_recall_diagnostics`) holds the original, owner-written
check, and `./shuashua` still prints it. `bench/bench_parity.cpp` lifts the same
logic into a standalone target so CI can gate on it. The duplication is deliberate:
`./shuashua` must stay a single "show me everything" dev command.

`bench_parity` adds three things the in-driver copy does not have:

- **a non-zero exit code** when the diff fails (the driver only prints `FAIL`
  and still returns 0, so it cannot gate anything);
- **an explicit statement of the kernel backend**, because off ARM there is no
  NEON and the diff compares a function against itself;
- **`--simulate-mismatch`**, which corrupts one value on purpose to prove the gate
  can actually go red. CI runs it. A check never observed failing is not a check.

## Reading the numbers honestly

**Three things will bite you if you quote a number without its context.**

1. **On any non-ARM target the parity result is meaningless.** `dot.hpp:49` guards
   the intrinsics on `__ARM_NEON`; elsewhere `dot_simd` *is* `dot_scalar`
   (`dot.hpp:86-90`). Every target prints its backend — read that line first.

2. **Harnesses differ between sections and must not be cross-subtracted.**
   `bench_parity` / `bench_dot` / `bench_recall` / `bench_layout` use **min of
   interleaved rounds** (noise only adds time, so the minimum is the reproducible
   figure, and interleaving means a thermal ramp hits both variants equally).
   `bench_pipeline` and `bench_wasm` use **means over many calls**, because there
   the point is comparing a total against its parts, which only works if both come
   from the same observations. Each section prints its own `harness:` line.

3. **The speedup depends on what else is in the loop.** `bench_dot` reports the
   same comparison at three granularities on purpose, because a bare kernel number
   and a "what the pipeline pays" number are both honest and differ substantially.
   Say which one you mean.

## The pinned run

`RESULTS.md` is a verbatim capture of one full run on a stated machine, with
hardware, compiler version, commit and date in the header. **README.md and
`docs/` quote that file and nothing else.** Re-running on a different laptop will
produce different absolute times; if you re-pin, replace `RESULTS.md` wholesale
and update anything that cites it, rather than editing individual numbers.

## Notes

- Build artifacts land in `bench/.build/` (git-ignored).
- No CMake, matching the rest of the project: `run_all.sh` invokes `clang++`
  directly. Override with `CXX=g++ bash bench/run_all.sh`.
- The WASM targets need `node`; the codegen probe needs `emcc` (and Binaryen's
  `wasm-dis`, which ships with Emscripten). Both skip cleanly if absent.
