# CLAUDE.md

Guidance for AI coding assistants (Claude Code, Cursor, etc.) working in this
repository. Read this before making changes.

## What this project is

Shua Shua is an educational **recommendation serving engine** written in C++ and
compiled to WebAssembly, presented behind a Xiaohongshu-style feed. It implements
the *serving* half of a recommender (the cascade-ranking pipeline: recall →
feature → score → rerank), organized as a DAG of operators, with per-stage tracing
so the pipeline is directly observable in the UI.

See `README.md` for the full design. This file governs *how* to work here.

## The single most important rule

**The core C++ engine is written by the repository owner, by hand.** This is a
portfolio project whose entire value is demonstrating that the owner can write a
C++ serving engine. If an assistant writes the engine, that value is destroyed.

Therefore:

- **Do NOT author or auto-complete the core engine logic.** Specifically off
  limits for generation: the operator implementations (`RecallOp`, `FeatureOp`,
  `ScoreOp`, `RerankOp`, `MixOp`), the DAG scheduler, the SIMD kernel, and the SoA
  data layout. The owner writes these.
- You **may**: explain concepts, review code the owner wrote, suggest structure
  or interfaces in prose, point out bugs, propose test cases, and help with
  scaffolding that is *not* the core (see below).
- When the owner asks "how should I structure X", answer with **guidance and
  interface sketches**, not a finished implementation they can paste. Leave the
  bodies for them to fill.
- If you are ever unsure whether something is "core," assume it is and ask.

> **Note — per-session exceptions.** The owner has, in specific sessions, explicitly
> authorized the assistant to implement the engine (all of M0–M2, and v2's C++). That
> authorization is **per session, not standing**: absent an explicit OK in the current
> session, the default above applies — don't write the core, ask first.

## What assistants may write freely

These are non-core and fine to generate:

- Build files: `CMakeLists.txt`, Emscripten flags, `Makefile`.
- The React frontend: masonry feed, the live profile panel (v2 replaced v1's
  persona switcher), card components, the DAG trace panel. (The frontend is
  presentation; use existing libraries — e.g. `react-masonry-css`/`masonic` for
  layout, a graph/flow lib for the DAG panel.)
- The offline synthetic-data generator script (Python or C++), since it is a
  fixture, not the engine.
- Glue at the WASM boundary (embind bindings) *once the owner has written the
  engine functions being bound* — bind what exists; do not invent engine logic.
- Docs, README updates, comments, `.gitignore`, CI config.

## Tech and conventions

- **Language:** C++20. Prefer standard library; avoid heavyweight dependencies in
  the engine.
- **Style:** clear over clever. This code is meant to be read (by the owner in an
  interview, and by others as a demo). No template metaprogramming for its own
  sake — it does not earn its keep in serving code and is off-brief.
- **Data layout:** item vectors are Structure-of-Arrays (one flat `float` buffer),
  not per-item vectors. Do not refactor to AoS.
- **No database, no network in the engine.** The item store is in-memory. The WASM
  build must remain a static page with no backend.
- **Operator interface is uniform:** every operator takes a batch and returns a
  batch, and records a trace entry `{ name, in_count, out_count, latency_us,
  sample_ids, detail }` (`detail` is an optional one-line note, e.g. MixOp's
  exploit/explore split). Preserve this contract; the UI depends on the trace shape.
- **Tracing is a first-class output.** Never optimize away or skip trace
  collection to "clean up" — the trace is the product.

## Build

```bash
# Native (kernel + full pipeline) — single file, no CMake needed
clang++ -std=c++20 -O2 src/main.cpp -o shuashua && ./shuashua

# WASM via Emscripten — a single emcc invocation (no CMake)
bash scripts/build-wasm.sh   # -> web/public/shuashua.js (single-file, wasm embedded)
```

Both native and WASM use single-command builds; there is no CMake step (a minimal
`CMakeLists.txt` exists only to quiet the IDE). **Rebuild the WASM after ANY engine
(C++) change** — `web/public/shuashua.js` is gitignored, so a stale one silently
breaks the app. Toolchain on the owner's machine: Apple clang (arm64), Emscripten
6.0.3 (brew), Node v26.

## Correctness discipline

- The `RecallOp` optimized (SIMD) path must produce output identical to the naive
  scalar path. Any change to either must keep the diff at zero. When touching
  either, run/verify the parity check.
- Keep the naive implementation around permanently as the reference; do not delete
  it once SIMD works — the diff check is a demo feature.

## Milestone awareness

v1 (kernel → DAG → SIMD+diff → WASM → feed UI) and v2 (the behavior-driven profile,
B1–B7 in `Doc/v2design.md`) are both **complete and pushed**; only ship (M5) remains.
The discipline still holds for new work: advance one increment at a time, and don't
pull speculative complexity (HNSW, quantization) into a smaller step. When in doubt,
do the smallest thing that advances the current goal.

## Scope discipline

Recommendation pipelines expand endlessly. The brief is a correct, believable
skeleton with depth concentrated in a few deliberate places: the SIMD recall kernel,
the DAG trace visualization, and (v2) the behavior-driven profile with its
exploration/exploitation mix. Do not add ranking models, extra operators, or training
code unless explicitly asked. Suggesting scope cuts is welcome; adding scope silently
is not.