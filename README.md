# Shua Shua: A Visual Recommendation Serving Engine in C++

> A browser-native recommendation engine. Shua Shua runs a real cascade-ranking pipeline — recall, feature extraction, scoring, reranking — as a DAG of C++ operators compiled to WebAssembly, and renders every stage as it happens behind a Xiaohongshu-style feed.

[![C++](https://img.shields.io/badge/C++-20-00599C?logo=cplusplus)](https://en.cppreference.com/)
[![WebAssembly](https://img.shields.io/badge/WebAssembly-Emscripten-654FF0?logo=webassembly)](https://emscripten.org/)
[![SIMD](https://img.shields.io/badge/SIMD-NEON%20%2F%20AVX2-FF6F00)](https://en.wikipedia.org/wiki/Single_instruction,_multiple_data)
[![React](https://img.shields.io/badge/React-18-61DAFB?logo=react)](https://react.dev/)
[![CMake](https://img.shields.io/badge/CMake-3.20+-064F8C?logo=cmake)](https://cmake.org/)
[![License](https://img.shields.io/badge/License-Apache%202.0-blue)](LICENSE)

> **Status (planning / early build):** the design below is the target. As of this
> commit the implementation is at the minimal-kernel stage (a single recall
> operator over synthetic notes, printed to stdout). Sections marked _(planned)_
> are not yet implemented. This README doubles as the build blueprint; status
> markers will be updated as operators land.

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
   │   │ Xiaohongshu-     │  (uid, hist)  │  DAG Scheduler     │  │
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
  point that returns the final items plus the execution trace as JSON. _(planned)_
- **React UI**: the Xiaohongshu-style masonry feed, persona switcher, per-card
  "why recommended" line, and a collapsible DAG panel that animates the trace.
  _(planned)_

---

## The Operator DAG

The cascade is a DAG of four operators. Cardinality numbers below are the target
demo shape (the "1,000,000" is the nominal candidate-pool size; recall works over
the in-memory store via an index rather than a full linear scan).

| Stage | Operator | In → Out (target) | What it does | Hot path |
|---|---|---|---|---|
| Recall | `RecallOp` | 1,000,000 → ~5,000 | Vector-similarity candidate generation | **SIMD inner product** |
| Feature | `FeatureOp` | ~5,000 → ~5,000 | Attach features (category match, recency, popularity) | — |
| Score | `ScoreOp` | ~5,000 → ~50 | Weighted multi-objective score (click / like / save) | — |
| Rerank | `RerankOp` | ~50 → ~12 | Diversity-aware reordering (MMR / DPP-style) | — |

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
    uint32_t    id;
    uint8_t     category;      // food / fashion / travel / tech ...
    float       popularity;    // like count, normalized
    // presentation-only fields (title, cover) kept in a parallel array
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

`RecallOp` ships in two implementations behind one interface:

- `RecallNaive` — plain scalar loop over all vectors.
- `RecallSimd` — hand-written SIMD inner product (NEON on Apple silicon, AVX2 on
  x86), _(planned)_ optionally over an HNSW index.

The engine can run both on the same input and assert a **diff**: identical output
ordering (parity), reported alongside the speedup. The UI exposes this as a toggle:

```
recall: naive  480 µs   |   recall: simd  32 µs   |   result diff = 0
```

This is the recommendation-serving analogue of pre/post-migration diff validation:
proving an optimization is faster *and* changes nothing about the result.

---

## Getting Started

### Prerequisites

- A C++20 compiler (Apple clang, GCC, or Clang)
- CMake 3.20+ _(optional at the kernel stage; a single `clang++` invocation works)_
- Emscripten _(only for the WASM build; not needed for the native kernel)_
- Node.js 20+ _(only for the frontend)_

### Native kernel (current stage)

The minimal kernel is a single translation unit that builds synthetic notes, runs
recall, and prints the top matches to stdout.

```bash
# Single-file build, no CMake needed yet
clang++ -std=c++20 -O2 src/main.cpp -o shuashua && ./shuashua
```

### Full build _(planned)_

```bash
# Native, with CMake once the source grows past one file
cmake -B build && cmake --build build && ./build/shuashua

# WASM, via Emscripten
emcmake cmake -B build-wasm && cmake --build build-wasm

# Frontend
cd web && npm install && npm run dev
```

---

## Performance Notes

_(targets, not yet measured)_

The hot path is the recall inner product: for each candidate, a dot product
between the user vector and the item vector. This is the one place C++ earns its
keep, and where SIMD applies directly.

| Lever | Idea | Status |
|---|---|---|
| SIMD inner product | Vectorize the dot product (NEON / AVX2) | planned |
| SoA layout | Column-wise vectors for contiguous streaming | planned |
| int8 quantization | 4× smaller vectors, cheaper loads | stretch |
| HNSW index | Sublinear recall so the store need not be scanned in full | stretch |

Memory budget: at `DIM=64` and `float32`, each vector is 256 bytes. 10k–50k items
is a few MB — trivial for a browser tab. The nominal "million-item pool" is served
via index rather than by loading a million vectors into a WASM heap.

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
| M0 — Kernel | `Note` struct, synthetic notes, one recall op, stdout | in progress |
| M1 — DAG | Operator interface, scheduler, four operators, trace output | planned |
| M2 — SIMD + diff | SIMD recall kernel, SoA layout, naive/simd parity check | planned |
| M3 — WASM | Emscripten build, `recommend` bound to JS, JSON trace | planned |
| M4 — Feed UI | Masonry feed, persona switch, "why", DAG trace panel | planned |
| M5 — Ship | Deploy static build to `shuashua.odieyang.com` | planned |
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

---

## License

[Apache License 2.0](LICENSE).