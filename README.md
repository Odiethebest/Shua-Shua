# Shua Shua: A Visual Recommendation Serving Engine in C++

> A browser-native recommendation engine. Shua Shua runs a real cascade-ranking pipeline — recall, feature extraction, scoring, reranking — as a DAG of C++ operators compiled to WebAssembly, and renders every stage as it happens behind a Xiaohongshu-style feed.

[![C++](https://img.shields.io/badge/C++-20-00599C?logo=cplusplus)](https://en.cppreference.com/)
[![WebAssembly](https://img.shields.io/badge/WebAssembly-Emscripten-654FF0?logo=webassembly)](https://emscripten.org/)
[![SIMD](https://img.shields.io/badge/SIMD-NEON%20%2F%20AVX2-FF6F00)](https://en.wikipedia.org/wiki/Single_instruction,_multiple_data)
[![React](https://img.shields.io/badge/React-18-61DAFB?logo=react)](https://react.dev/)
[![Vite](https://img.shields.io/badge/Vite-6-646CFF?logo=vite)](https://vitejs.dev/)
[![License](https://img.shields.io/badge/License-Apache%202.0-blue)](LICENSE)

> **Status:** built and working end-to-end. Milestones M0–M4 are complete — the
> C++ serving engine (SoA item store, a four-operator DAG with per-stage tracing,
> and a hand-written NEON recall kernel checked for parity against a scalar
> reference), compiled to WebAssembly, behind a React feed with a live DAG trace
> panel, persona switcher, dark mode, and build-time cover images. M5 (deploy) is
> pending. Full design docs: [`Doc/en`](Doc/en) · [`Doc/ch`](Doc/ch).

---

## Documentation

Full engineering docs live in [`Doc/en`](Doc/en) (English) and [`Doc/ch`](Doc/ch) (中文):

- [Architecture](Doc/en/Architecture.md) — system overview, components, request lifecycle, key decisions
- [Core Design](Doc/en/Core_Design.md) — the C++ engine: data model, operators, scheduler, SIMD kernel, API boundary
- [Frontend Design](Doc/en/Frontend_Design.md) — the React app, engine integration, theming, trace visualization
- [Operations](Doc/en/Operations.md) — build, run, verify, deploy; roadmap & status
- [Engine internals (study notes)](Doc/en/algo.md) — every component in plain language, the design WHYs, and the terms an interviewer might probe

---

## Table of Contents

- [Overview](#overview)
- [Core Idea](#core-idea)
- [Architecture](#architecture)
- [The Operator DAG](#the-operator-dag)
- [Data Model](#data-model)
- [Consistency Check](#consistency-check)
- [Getting Started](#getting-started)
- [Performance Notes](#performance-notes)
- [Key Design Decisions](#key-design-decisions)
- [Roadmap](#roadmap)
- [FAQ](#faq)

---

## Overview

Industrial recommendation systems split into two halves. **Training** happens
offline in Python: it learns which content suits which user and exports item
embeddings and model weights. **Serving** happens online, usually in C++: when a
user opens the app, it must turn a candidate pool of millions into a handful of
ranked items in single-digit milliseconds. Shua Shua is a self-contained,
educational implementation of the **serving half** — the part that is most
C++-bound and least visible.

The problem with a serving engine is that it is, by nature, an invisible backend.
Shua Shua's goal is to make it legible: the same request that produces a feed
also produces an execution trace, and the frontend renders that trace as a
funnel of DAG operators lighting up one after another.

| Concern | Approach |
|---|---|
| Candidate generation | Vector similarity recall over an in-memory item store |
| Ranking cascade | Recall → feature → multi-objective score → diversity rerank, as an operator DAG |
| Per-stage observability | Every operator reports input count, output count, and latency into a trace |
| Hot-path performance | Hand-written SIMD inner-product kernel; Structure-of-Arrays item layout |
| Correctness under optimization | Naive vs. optimized operator diff check (parity + speedup) |
| Zero-backend demo | Engine compiled to WebAssembly; runs entirely in the browser |
| Believable synthetic data | Category-centroid vectors so recall is semantically meaningful |

---

## Core Idea

> **A recommendation serving engine is a directed acyclic graph of operators
> executed over an in-memory candidate store. If each operator is made to report
> its own latency and cardinality, the otherwise-invisible ranking cascade becomes
> directly observable — without changing what the engine computes.**

Everything in the engine is an **Operator**: it takes a batch of candidate items
in, returns a (usually smaller) batch out, and records what it did. The recommend
pipeline is just a sequence of operators wired into a DAG and executed in
topological order by a scheduler. There are no hard-coded "recall function" or
"ranking function" — only operators and the graph that connects them.

This mirrors how feature-computation and serving platforms are actually built,
where feature logic is expressed as a DAG of operators and the engine is a
scheduler over that graph.

---

## Architecture

```
   Browser (single static page, no server)
   ┌───────────────────────────────────────────────────────────┐
   │                                                             │
   │   React UI                          WASM module (C++)       │
   │   ┌─────────────────┐   recommend   ┌────────────────────┐  │
   │   │ Xiaohongshu-     │  (persona)    │  DAG Scheduler     │  │
   │   │ style feed       │ ────────────▶ │  topological exec  │  │
   │   │  + persona switch│               └─────────┬──────────┘  │
   │   │  + "why" reasons │               ┌─────────▼──────────┐  │
   │   │                  │               │  Operators:        │  │
   │   │ DAG panel        │ ◀──────────── │  Recall → Feature  │  │
   │   │  (feed + trace)  │  feed+trace   │  → Score → Rerank  │  │
   │   └─────────────────┘  (JSON)        └─────────┬──────────┘  │
   │                                      ┌─────────▼──────────┐  │
   │                                      │ In-memory item     │  │
   │                                      │ store (SoA vectors)│  │
   │                                      └────────────────────┘  │
   └───────────────────────────────────────────────────────────┘
```

**Components**

- **DAG Scheduler** (C++): holds the operator graph, executes nodes in
  topological order, collects a per-node trace.
- **Operators** (C++): `RecallOp`, `FeatureOp`, `ScoreOp`, `RerankOp` — each a
  self-contained unit with a uniform `run(batch) -> batch` interface.
- **Item Store** (C++): item vectors held in memory as Structure-of-Arrays for
  cache- and SIMD-friendly access. This is the serving store; no database.
- **WASM boundary** (Emscripten + embind): exposes a single `recommend` entry
  point that returns the final feed plus the execution trace as JSON.
- **React UI**: a Xiaohongshu web–style layout — left sidebar with a persona
  switcher, a responsive masonry feed with a per-card "why recommended" line and
  build-time cover images, a collapsible DAG trace panel, and a dark-mode toggle.

---

## The Operator DAG

The cascade is a DAG of four operators. Cardinalities below are the current demo
shape over a 3,000-note in-memory store. The design scales to a nominal
million-item pool served via an index; today recall does a full linear scan.

| Stage | Operator | In → Out | What it does | Hot path |
|---|---|---|---|---|
| Recall | `RecallOp` | 3,000 → 300 | Vector-similarity candidate generation | **SIMD inner product** |
| Feature | `FeatureOp` | 300 → 300 | Attach features (category match, recency, popularity) | — |
| Score | `ScoreOp` | 300 → 50 | Weighted multi-objective score (click / like / save) | — |
| Rerank | `RerankOp` | 50 → 12 | Diversity-aware reordering (MMR) | — |

Each operator emits a trace record: `{ name, in_count, out_count, latency_us,
sample_ids }`. The scheduler concatenates these into the trace the UI animates.

---

## Data Model

There is **no database**. All item data is generated offline once and loaded into
memory at startup. This is deliberate: a serving engine keeps its candidate store
resident in memory for microsecond access, exactly as production ANN serving does.
Databases belong to the offline feature-production path, not the serving hot path.

An item (a "note") looks like:

```cpp
struct Note {
    uint32_t    id;            // stable id (== index into the store)
    uint8_t     category;      // food / fashion / travel / tech ...
    float       popularity;    // like count, normalized to [0, 1]
    uint32_t    age_days;      // drives the recency feature
    // presentation-only fields (title, cover, author) are synthesized in the UI
};
```

Vectors are stored **column-wise** (Structure-of-Arrays), not per-item, so the
similarity kernel can stream contiguous memory:

```cpp
// Not: std::vector<Note> where each Note carries its own vector (AoS)
// But:  one flat buffer, all vectors concatenated (SoA)
std::vector<float> item_vectors;   // size = num_items * DIM, row-major by item
size_t DIM = 64;                   // small, fixed embedding dimension
```

**Synthetic data recipe (the part that makes the demo believable):** each category
has a centroid vector; each note's vector is `centroid[category] + small_noise`.
Same-category notes therefore cluster in vector space, so similarity recall is
genuinely meaningful — pick a "food" persona and recall returns food. The data is
fabricated; the ranking mechanism is real.

---

## Consistency Check

> Mirrors the "prove the optimized path matches the naive path" discipline of
> real serving-system migrations.

Recall ships in two implementations behind one signature:

- `recall_naive` — a plain scalar loop; the permanent reference.
- `recall_simd` — a hand-written NEON inner product (`dot_simd`). On non-NEON
  targets (including the WASM build) it falls back to the scalar kernel.

The engine runs both on the same input and asserts a **diff**: the top-k ranking
is identical (parity), reported alongside the speedup. The native build
(`./shuashua`) prints it:

```
dot scan:  naive 62us | simd 17us | speedup 3.6x
result diff = 0   (top-300 ranking identical; max score delta ~3e-7)
```

This is the recommendation-serving analogue of pre/post-migration diff validation:
proving an optimization is faster *and* changes nothing about the result.

---

## Getting Started

### Prerequisites

- A C++20 compiler (Apple clang, GCC, or Clang) — for the native build
- Emscripten (`emcc`) — for the WebAssembly build
- Node.js 20+ — for the frontend and build scripts

### Native engine (build + parity check)

```bash
clang++ -std=c++20 -O2 src/main.cpp -o shuashua && ./shuashua
```

Builds synthetic data, runs the pipeline, and prints the feed, the DAG trace, the
`recommend()` JSON, and the naive-vs-SIMD recall parity + speedup.

### WebAssembly + frontend

```bash
# 1. compile the engine to a single-file WASM module (web/public/shuashua.js)
scripts/build-wasm.sh                  # needs emcc on PATH
node scripts/wasm_smoke.mjs            # optional: verify it in Node

# 2. (optional) fetch cover images once, into the repo
UNSPLASH_KEY=your_key node scripts/fetch-covers.mjs

# 3. run the frontend
cd web && npm install && npm run dev   # http://localhost:5173
```

Without an Unsplash key the feed still works — covers fall back to gradient
placeholders. See [Operations](Doc/en/Operations.md) for details and deployment.

---

## Performance Notes

The hot path is the recall inner product: for each candidate, a dot product
between the query vector and the item vector. This is the one place C++ earns its
keep, and where SIMD applies.

| Lever | Idea | Status |
|---|---|---|
| SIMD inner product | Hand-written NEON dot product (`dot_simd`) | done — ~3.6× on the scan |
| SoA layout | Column-wise vectors for contiguous streaming | done |
| int8 quantization | 4× smaller vectors, cheaper loads | stretch |
| HNSW index | Sublinear recall so the store need not be scanned in full | stretch |

Latencies are measured natively; in the browser they are real microseconds only
when the page is cross-origin-isolated (COOP/COEP). Memory budget: at `DIM=64`
and `float32` each vector is 256 bytes, so the 3,000-note demo store is under
1 MB. A nominal million-item pool would be served via an index rather than by
loading every vector into the WASM heap.

---

## Key Design Decisions

**Everything is an Operator.**
A uniform `run(batch) -> batch` interface plus a scheduler over a DAG. This is what
lets the pipeline be both extended (add a node) and observed (every node traces
itself) without special-casing any stage.

**The engine returns a trace, not just a result.**
Observability is a first-class output, designed in from the start rather than
bolted on. The trace is the bridge that turns an invisible backend into a
one-glance demo.

**No database; the item store is resident in memory.**
This is not a shortcut — it is what a serving hot path looks like. Vectors live in
memory as SoA; persistence and feature production are explicitly out of scope for
the serving half.

**Compile to WASM instead of standing up a C++ HTTP service.**
Removes the network seam entirely, makes the whole thing a static page anyone can
open, and turns "C++ recommendation engine running in your browser" into the demo.
The tradeoff is the Emscripten toolchain and a browser memory ceiling, addressed
by the in-memory / index split above.

**Synthetic data with category centroids.**
Fabricated content, real clustering, so recall behaves meaningfully without any
training pipeline. The offline training half (learned embeddings) is a possible
future extension, not a dependency.

---

## Roadmap

| Milestone | Contents | Status |
|---|---|---|
| M0 — Kernel | `Note` struct, synthetic notes, one recall op, stdout | done |
| M1 — DAG | Operator interface, scheduler, four operators, trace output | done |
| M2 — SIMD + diff | NEON recall kernel, SoA layout, naive/simd parity check | done |
| M3 — WASM | Emscripten build, `recommend` bound to JS, JSON trace | done |
| M4 — Feed UI | Masonry feed, persona switch, "why", DAG trace panel, dark mode, build-time covers | done |
| M5 — Ship | Deploy static build to `shuashua.odieyang.com` | pending |
| Stretch | HNSW index, int8 quantization, learned embeddings | later |

---

## FAQ

**Is this a real recommendation system?**
It implements the serving half — the cascade-ranking pipeline that turns a
candidate pool into a ranked feed — with real vector recall, multi-objective
scoring, and diversity reranking. It does not learn embeddings; those are
synthesized. That split (serve vs. train) is intentional and matches how the two
halves are separated in practice.

**Why no database?**
A serving engine keeps its candidate store in memory for microsecond access.
Querying a database on the request path would defeat the purpose. Databases live
in the offline feature-production path, which is out of scope here.

**Why WebAssembly?**
So the whole engine runs in the browser with no backend, and the demo is a single
link anyone can open. It also makes for an unusual, honest technical story:
a C++ serving engine compiled to WASM.

**Where do the cover photos come from?**
They are fetched from Unsplash once at build time and committed to the repo (with
photographer attribution). At runtime the site makes no Unsplash API calls and
ships no API key. Without a key, covers fall back to gradient placeholders.

---

## License

[Apache License 2.0](LICENSE).