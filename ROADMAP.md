# ROADMAP — Shua Shua engine build

A checkable execution plan for building the Shua Shua serving engine, **engine
first**, one milestone at a time. This tracks *execution*; `README.md` holds the
*design* and `CLAUDE.md` holds the repo *governance*.

Legend: `- [ ]` not started · `- [~]` in progress · `- [x]` done.

---

## How we work this build

For this build the owner has authorized implementing the **core** engine
(operators, DAG scheduler, similarity kernel, SoA access) — normally reserved for
the owner under `CLAUDE.md`. That authorization is scoped to this engine-build
push; the default in `CLAUDE.md` otherwise stands. In exchange, three rules
govern every line written:

1. **Readability over cleverness.** The simplest correct version a mid-level
   engineer would write. No template metaprogramming, no obscure STL tricks, no
   premature micro-optimization. A plain five-line loop beats a clever one-liner.
2. **SIMD recall kernel, specifically.** The naive scalar path is the *primary*,
   clearly-commented implementation. The SIMD path stays minimal and heavily
   commented — *why* this width, *how* the tail/remainder is handled, *why* NEON
   on arm64 — with no intrinsics beyond a basic dot product. Naive and SIMD must
   produce identical output.
3. **Comments explain WHY, not WHAT.** Every non-obvious block documents the
   design decision and the alternative that was rejected and why. Comments are
   for a reader learning the system, not for brevity.

These sit on top of the `CLAUDE.md` rules that remain in force (SoA layout, the
uniform operator interface, the fixed trace shape, tracing as a first-class
output, in-memory store with no DB/network, and no scope creep).

**Cadence:** stay buildable at every step with the documented command; after each
milestone, stop, show the owner what was written, confirm it builds and runs, and
wait before starting the next.

```bash
clang++ -std=c++20 -O2 src/main.cpp -o shuashua && ./shuashua
```

---

## Definition of Done — applies to every milestone

- [ ] Builds and runs via `clang++ -std=c++20 -O2 src/main.cpp -o shuashua && ./shuashua`
- [ ] Clean under `-Wall -Wextra` (no warnings)
- [ ] Readability rule honored — no TMP, no obscure tricks, no premature micro-opt
- [ ] WHY-comments on every non-obvious block (decision + rejected alternative)
- [ ] SoA layout preserved; operator/trace contract `{name, in_count, out_count, latency_us, sample_ids}` preserved
- [ ] Shown to the owner; build/run confirmed; waited for the go-ahead before continuing

---

## Progress

### Groundwork — scaffolding (done, committed)

- [x] Project layout: `src/`, `web/`, `scripts/`, `.gitignore`
- [x] Interface & data declarations: `Note`, `Batch`, `TraceEntry`, `Operator`, `ItemStore` SoA fields
- [x] Operator/scheduler headers as declarations-only stubs
- [x] Placeholder `main.cpp` builds clean under `-std=c++20 -Wall -Wextra`
- [x] Minimal `CMakeLists.txt` for IDE indexing (single `shuashua` target)
- [x] Repo initialized and pushed to `origin/main` (rebased onto the existing `LICENSE`)

### M0 — Kernel  ·  _status: done_

Goal: one naive recall over a synthetic in-memory store, printed to stdout.

- [x] `ItemStore::count()` and `vector_of()` — SoA access (row-major, `i * DIM`)
- [x] Synthetic data (fixture): per-category centroid vectors + small per-note noise, built in memory
- [x] Naive scalar recall similarity (dot product) over the SoA buffer (`recall_naive`)
- [x] `main.cpp`: build the store, run recall for a query/persona, print top-N matches
- [x] Confirm same-category notes cluster (food query returns 10 food notes, sim ~0.86–0.88)
- [x] Definition of Done satisfied

### M1 — DAG  ·  _status: done_

Goal: all four operators wired through the scheduler, each emitting a trace.

- [x] `RecallOp` produces a `Batch` + `TraceEntry` (via the base `run` template-method wrapping `transform`)
- [x] `FeatureOp` — attach features (category match, recency, popularity); cardinality unchanged
- [x] `ScoreOp` — transparent weighted blend over the features; keep top-k
- [x] `RerankOp` — greedy MMR with category-diversity redundancy; emit final page
- [x] `DagScheduler` — hold nodes, execute in order, collect the trace sequence
- [x] Wire `Recall → Feature → Score → Rerank`; print the final feed + full trace
- [x] Trace shape `{name, in_count, out_count, latency_us, sample_ids}` preserved end to end
- [x] Definition of Done satisfied

### M2 — SIMD + diff  ·  _status: done_

Goal: a SIMD recall path proven identical to the naive path, and faster.

- [x] SIMD recall kernel (NEON on arm64, 4-wide), minimal and heavily commented (`dot_simd` in `dot.hpp`)
- [x] Tail / remainder handling documented (scalar tail loop covers `dim % 4`; no-op at DIM=64 but correct for any dim)
- [x] Naive and SIMD behind one interface (`RecallKernel` on `RecallOp`); `recall_naive` kept permanently as the reference
- [x] Parity check: `diff = 0` (top-300 ranking identical, same item set; max score delta ~3e-07)
- [x] Report naive vs. SIMD latency + speedup (scan ~3.6x; end-to-end recall ~1.9x with the shared sort)
- [x] Definition of Done satisfied

### M3 — WASM  ·  _status: done_

Goal: the engine compiled to WebAssembly, exposing `recommend` to JS with a JSON feed + trace.

- [x] Single `recommend(persona)` entry point + `to_json` (feed + trace) in `api.hpp`; native driver in `main.cpp`
- [x] Resident store singleton (built once, reused per request)
- [x] embind bindings (`recommend` / `personaCount` / `personaLabel`) in `bindings.cpp`
- [x] Single-command `emcc` build (`scripts/build-wasm.sh`) → `web/public/shuashua.js` (single-file, wasm embedded; no CMake needed)
- [x] Verified in Node (`scripts/wasm_smoke.mjs`): valid JSON, funnel trace, persona list
- [x] WASM (scalar fallback) matches native (NEON) recommendations exactly — same order, scores identical to JSON precision
- [x] Definition of Done: native driver builds/runs clean under `clang++ -std=c++20 -O2 -Wall -Wextra`; WASM builds via `emcc`

### M4 — Feed UI  ·  _status: done_

Goal: a Xiaohongshu-style feed over the WASM engine, with the DAG trace made visible.

- [x] Vite + React + TS app in `web/`, loads the WASM module at runtime
- [x] Two-column `react-masonry-css` feed of varying-height cards (gradient + emoji covers, title, author, likes)
- [x] Persona switcher (tabs; active in the accent color) — re-runs `recommend()` on switch
- [x] Per-card "why recommended" line, derived from the engine's real feature values
- [x] Collapsible DAG trace panel: 4-stage funnel (in→out, latency, sample_ids) with a staggered reveal
- [x] Cross-origin isolation headers so the trace latencies are real high-res microseconds
- [x] `npm run build` clean (tsc + vite); rendered and verified via headless screenshot

---

## Deferred — later

- [ ] M5 — Ship: deploy the static build (host must send the COOP/COEP headers)
- [ ] Stretch: HNSW index, int8 quantization, learned embeddings, WASM SIMD recall
