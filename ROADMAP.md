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

### M2 — SIMD + diff  ·  _status: not started_

Goal: a SIMD recall path proven identical to the naive path, and faster.

- [ ] SIMD recall kernel (NEON on arm64), minimal and heavily commented
- [ ] Tail / remainder handling documented (why, and how the leftover lanes are covered)
- [ ] Naive and SIMD behind one interface; naive kept permanently as the reference
- [ ] Parity check: assert identical output (`diff = 0`)
- [ ] Report naive vs. SIMD latency and the speedup
- [ ] Definition of Done satisfied

---

## Deferred — not this session

Engine first. These come later and are explicitly out of scope for now.

- [ ] M3 — WASM: Emscripten build, `recommend` bound to JS, JSON trace
- [ ] M4 — Feed UI: masonry feed, persona switch, "why", DAG trace panel
- [ ] M5 — Ship: deploy the static build
- [ ] Stretch: HNSW index, int8 quantization, learned embeddings
