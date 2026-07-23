# Architecture

## 1. Purpose and scope

Shua Shua is an educational **recommendation serving engine**. Industrial
recommenders split into two halves: **training** (offline, in Python — learns
embeddings and model weights) and **serving** (online, usually in C++ — turns a
large candidate pool into a small ranked feed in milliseconds). Shua Shua
implements the *serving* half, and makes it **observable**: the same request that
produces a feed also produces an execution trace, rendered in the UI as a funnel
of pipeline stages.

The engine is written in C++20, compiled to WebAssembly with Emscripten, and
presented behind a Xiaohongshu-style feed built in React. The whole product is a
static site with **no backend and no network calls on the request path**.

## 2. System context

```
   Browser (single static page, no server)
   ┌──────────────────────────────────────────────────────────────┐
   │                                                                │
   │   React UI (web/)                    WASM module (src/ → C++)   │
   │   ┌────────────────────┐  recommend  ┌───────────────────────┐ │
   │   │ Sidebar personas   │  (persona)  │  DAG Scheduler         │ │
   │   │ Waterfall feed     │ ──────────▶ │  topological execution │ │
   │   │ DAG trace panel    │             ├───────────────────────┤ │
   │   │                    │  JSON feed  │  Operators:            │ │
   │   │                    │ ◀────────── │  Recall→Feature→       │ │
   │   │                    │  + trace    │  Score→Rerank          │ │
   │   └────────────────────┘             ├───────────────────────┤ │
   │                                       │ In-memory item store   │ │
   │                                       │ (SoA float vectors)    │ │
   │                                       └───────────────────────┘ │
   └──────────────────────────────────────────────────────────────┘
```

- **React UI** (`web/`): the presentation layer. Loads the compiled engine,
  calls `recommend(persona)`, and renders both the resulting feed and the
  per-operator trace. Cover images are static assets fetched at build time.
- **WASM module** (`src/`, compiled): the engine. Exposes a single `recommend`
  entry point (plus persona metadata) through embind. Everything runs in the
  browser tab.

## 3. Runtime component model

| Component | Location | Responsibility |
|---|---|---|
| DAG scheduler | `src/scheduler.hpp` | Holds operators as nodes, executes them in order, threads each stage's output into the next, collects the trace. |
| Operators | `src/{recall,feature,score,rerank}_op.hpp` | Self-contained pipeline stages with a uniform interface (batch in → batch out, plus one trace record). |
| Item store | `src/item_store.hpp` | In-memory candidate store. Item vectors held Structure-of-Arrays (one flat `float` buffer) for cache- and SIMD-friendly access. |
| Similarity kernel | `src/dot.hpp` | The hot inner-product: a scalar reference and a hand-written NEON path. |
| Synthetic data | `src/synthetic.hpp` | Builds the in-memory store from category-centroid vectors (a fixture standing in for learned embeddings). |
| API / boundary | `src/api.hpp`, `src/bindings.cpp` | `recommend(persona)` orchestration, JSON serialization, and the embind bindings exposed to JS. |
| Frontend | `web/src/**` | React app: sidebar, waterfall feed, trace panel, engine loader, presentation layer. |

## 4. The operator DAG (request path)

The cascade is a DAG of four operators. Cardinalities below are the demo shape
(store = 3,000 synthetic notes):

| Stage | Operator | In → Out | What it does | Hot path |
|---|---|---|---|---|
| Recall | `RecallOp` | 3,000 → 300 | Vector-similarity candidate generation | **SIMD inner product** |
| Feature | `FeatureOp` | 300 → 300 | Attach features (category match, recency, popularity) | — |
| Score | `ScoreOp` | 300 → 50 | Weighted multi-objective score, keep top-k | — |
| Rerank | `RerankOp` | 50 → 12 | Diversity-aware reorder (MMR), emit the page | — |

Every operator emits a trace record `{ name, in_count, out_count, latency_us,
sample_ids }`. The scheduler concatenates these into the trace the UI animates.
Tracing is a **first-class output**, designed in rather than bolted on.

## 5. Request lifecycle

1. UI calls `recommend(personaId)` across the WASM boundary.
2. `api.hpp` resolves the persona to per-category interest weights, builds a
   unit-normalized query vector from the category centroids, and assembles the
   operator pipeline (`RecallOp → FeatureOp → ScoreOp → RerankOp`).
3. The `DagScheduler` seeds the pipeline with the full candidate pool and runs
   each operator in order, collecting a `TraceEntry` per stage.
4. `to_json` serializes the final feed plus the trace to a JSON string.
5. The UI parses it, renders the masonry feed (with local cover images and an
   engine-derived "why recommended" line), and animates the DAG trace funnel.

The item store is built **once** on first use (a resident singleton) and reused
across requests — there is no per-request data load, matching how a production
serving store stays memory-resident.

## 6. Key architectural decisions

- **Everything is an operator.** A uniform `run(batch) → batch` interface plus a
  scheduler over a DAG. This is what lets the pipeline be both extended (add a
  node) and observed (every node traces itself) without special-casing a stage.
- **The engine returns a trace, not just a result.** Observability is a designed
  output; it is the bridge that turns an invisible backend into a one-glance demo.
- **In-memory Structure-of-Arrays store; no database, no network.** Vectors live
  in one flat buffer so the similarity kernel streams contiguous memory. This is
  what a serving hot path looks like; persistence and feature production belong to
  the (out-of-scope) offline half.
- **Compile to WebAssembly instead of standing up a C++ HTTP service.** Removes
  the network seam entirely and makes the whole thing a static page.
- **Correctness under optimization.** The SIMD recall path must produce output
  identical to the naive scalar reference; a parity check reports the diff (zero)
  alongside the speedup. The naive path is kept permanently as the reference.
- **Static assets, no runtime third-party calls.** Cover images are fetched from
  Unsplash once at build time and committed; at runtime the site makes zero
  Unsplash API calls and ships no API key.

## 7. Cross-cutting concerns

- **Determinism.** Synthetic data uses a fixed PRNG seed; ranking tie-breaks are
  resolved by id. Runs — and the naive/SIMD parity check — are reproducible.
- **Cross-origin isolation.** The site is served with `COOP: same-origin` and
  `COEP: require-corp`, which unlock the browser's high-resolution timer so the
  per-operator trace latencies are real microseconds. All resources are
  same-origin (engine + local covers), so isolation is straightforward.
- **Portability of the kernel.** The SIMD path is guarded by `__ARM_NEON`; on
  targets without NEON (including the WASM build) it falls back to the scalar
  kernel, so behavior is correct everywhere and only performance differs.

## 8. Technology stack

- **Engine:** C++20, standard library only; Emscripten + embind for the WASM
  build; ARM NEON intrinsics for the recall kernel.
- **Frontend:** Vite, React 18, TypeScript, `react-masonry-css`.
- **Tooling:** Apple clang (native builds and the parity check), Node.js (build
  scripts and a headless smoke test).

See [Core_Design](Core_Design.md) for the engine internals,
[Frontend_Design](Frontend_Design.md) for the UI, and
[Operations](Operations.md) for build, run, and deployment.
