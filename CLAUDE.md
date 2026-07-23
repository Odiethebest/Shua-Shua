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
  `ScoreOp`, `RerankOp`), the DAG scheduler, the SIMD kernel, and the SoA data
  layout. The owner writes these.
- You **may**: explain concepts, review code the owner wrote, suggest structure
  or interfaces in prose, point out bugs, propose test cases, and help with
  scaffolding that is *not* the core (see below).
- When the owner asks "how should I structure X", answer with **guidance and
  interface sketches**, not a finished implementation they can paste. Leave the
  bodies for them to fill.
- If you are ever unsure whether something is "core," assume it is and ask.

## What assistants may write freely

These are non-core and fine to generate:

- Build files: `CMakeLists.txt`, Emscripten flags, `Makefile`.
- The React frontend: masonry feed, persona switcher, card components, the DAG
  trace panel. (The frontend is presentation; use existing libraries — e.g.
  `react-masonry-css`/`masonic` for layout, a graph/flow lib for the DAG panel.)
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
  sample_ids }`. Preserve this contract; the UI depends on the trace shape.
- **Tracing is a first-class output.** Never optimize away or skip trace
  collection to "clean up" — the trace is the product.

## Build

```bash
# Native kernel stage — single file, no CMake needed
clang++ -std=c++20 -O2 src/main.cpp -o shuashua && ./shuashua

# Later: native via CMake
cmake -B build && cmake --build build

# Later: WASM via Emscripten
emcmake cmake -B build-wasm && cmake --build build-wasm
```

Toolchain present on the owner's machine: Apple clang (arm64), make at
`/usr/bin/make`. CMake and Emscripten are not installed yet and are not required
until the DAG/WASM milestones.

## Correctness discipline

- The `RecallOp` optimized (SIMD) path must produce output identical to the naive
  scalar path. Any change to either must keep the diff at zero. When touching
  either, run/verify the parity check.
- Keep the naive implementation around permanently as the reference; do not delete
  it once SIMD works — the diff check is a demo feature.

## Milestone awareness

Work in milestone order (see README Roadmap): kernel → DAG → SIMD+diff → WASM →
feed UI → ship. Do not pull later-milestone complexity (WASM, HNSW, quantization)
into earlier milestones. When in doubt, do the smallest thing that advances the
current milestone.

## Scope discipline

Recommendation pipelines expand endlessly. The brief is: a correct, believable
skeleton with depth concentrated in **one** place (the SIMD recall kernel + the
DAG trace visualization). Do not add ranking models, extra operators, or training
code unless explicitly asked. Suggesting scope cuts is welcome; adding scope
silently is not.