# Core Engine Design

This document describes the C++ engine in `src/`. It assumes the system overview
in [Architecture](Architecture.md).

## 1. Data model

### 1.1 Note (`note.hpp`)

An item ("note") carries only the scalar signals the cascade reads:

```cpp
struct Note {
    std::uint32_t id;         // stable item id (== index into the store)
    std::uint8_t  category;   // food / fashion / travel / tech / ...
    float         popularity; // normalized to [0, 1]
    std::uint32_t age_days;   // drives the recency feature
};
```

Presentation fields (title, cover, author) are intentionally **not** here — they
are synthesized in the frontend.

### 1.2 Item store (`item_store.hpp`)

The store is **Structure-of-Arrays**: one flat `float` buffer holding every item
vector concatenated, row-major by item, plus a parallel `notes` array.

```cpp
struct ItemStore {
    static constexpr std::size_t DIM = 64;   // fixed embedding dimension
    std::vector<float> embeddings;           // count()*DIM, row-major (SoA)
    std::vector<Note>  notes;                // parallel metadata
    std::size_t  count() const;              // embeddings.size() / DIM
    const float* vector_of(std::uint32_t i); // &embeddings[i*DIM]
};
```

**Why SoA:** the similarity kernel walks item vectors as contiguous memory, which
is what makes cache behavior good and the SIMD path worthwhile. `vector_of`
returns a raw pointer into the existing buffer — no copy. The layout is a
contract; it is not refactored to array-of-structs.

### 1.3 Synthetic data (`synthetic.hpp`)

A fixture that stands in for learned embeddings. Each category has a random
centroid; each note's vector is `centroid[category] + small noise`, then unit-
normalized. Same-category notes therefore cluster, so similarity recall is
semantically meaningful. A fixed PRNG seed makes the store reproducible.

Normalizing to unit length makes the dot product equal to cosine similarity, so
"similar" means "same direction" (same category) regardless of magnitude — and
the kernel stays a plain dot product.

## 2. The operator interface (`operator.hpp`)

`Batch` is the payload that flows between stages — an array-of-structs of
`Candidate` (the batch is small and transient, so AoS reads clearly; the SoA rule
is only for the item store / hot kernel):

```cpp
struct Candidate {
    std::uint32_t id;
    float similarity, category_match, recency, popularity, score; // filled progressively
};
struct Batch { std::vector<Candidate> items; };
```

`TraceEntry` is the fixed record every stage emits (the UI depends on this shape):

```cpp
struct TraceEntry {
    std::string name; std::size_t in_count, out_count;
    double latency_us; std::vector<std::uint32_t> sample_ids;
};
```

`Operator` uses the **template-method** pattern: the base `run()` is defined
once — it times the stage, calls the subclass `transform()`, and appends exactly
one `TraceEntry`. Subclasses implement only `transform()` and `name()`.

```cpp
class Operator {
public:
    virtual std::string name() const = 0;
    Batch run(const Batch& in, std::vector<TraceEntry>& trace) const; // times + records
protected:
    virtual Batch transform(const Batch& in) const = 0;               // the algorithm
};
```

**Why centralize tracing in the base:** the `{name, in, out, latency_us,
sample_ids}` contract is produced identically for every stage and cannot drift or
be forgotten. Tracing is the product, so it is enforced in one place.

## 3. The four operators

- **RecallOp** (`recall_op.hpp`) — source stage. Scores every item in the store
  by dot-product similarity to the query and keeps the top-k. It streams the
  contiguous SoA buffer (not an input id list), which is what makes the SIMD
  kernel pay off. Owns a kernel selector (scalar or SIMD).
- **FeatureOp** (`feature_op.hpp`) — attaches features per candidate:
  `category_match` (the persona's affinity for the item's category), `recency`
  (exponential decay of `age_days`), `popularity` (passthrough). Cardinality
  unchanged.
- **ScoreOp** (`score_op.hpp`) — a transparent weighted blend of the features
  into one score, then keep the top-k. A production ranker would blend learned
  per-objective models (pCTR/pLike/pSave); with no trained models here, the
  linear blend is honest about what it is.
- **RerankOp** (`rerank_op.hpp`) — greedy MMR (maximal marginal relevance) using
  category as the redundancy axis: `value = λ·score − (1−λ)·redundancy`, so the
  final page trades relevance against category diversity.

## 4. The scheduler (`scheduler.hpp`)

`DagScheduler` holds operators and runs them in insertion order, threading each
stage's output batch into the next and collecting the trace. The Shua Shua
cascade is a linear chain, so this is a degenerate DAG (one path, no branches).

**Why not a general DAG engine (topological sort, multi-input nodes) yet:** no
branching pipeline exists to schedule, so it would be speculative complexity with
undefined semantics (e.g. merging two input batches). It grows into topological
execution if and when a branching pipeline appears.

## 5. The similarity kernel and parity (`dot.hpp`)

The one hot computation is the dot product of the query with each item vector.

- `dot_scalar` — the reference. A plain left-to-right accumulate. It stays scalar
  even at `-O2` because clang will not auto-vectorize a floating-point reduction
  without `-ffast-math` (reassociation changes results).
- `dot_simd` — hand-written NEON. Processes 4 floats per iteration (a 128-bit
  register = 4× `float32`), keeps four lane accumulators, then horizontally sums
  them. A scalar tail loop covers `dim % 4` (a no-op at DIM=64, but keeps the
  kernel correct for any dimension). Guarded by `__ARM_NEON`; falls back to
  `dot_scalar` elsewhere (including WASM).

Recall routes both kernels through a shared `score_all` + `rank_topk`, so the
naive/SIMD comparison isolates exactly the kernel. Because SIMD sums in a
different order, scores differ at the floating-point-reassociation level
(~1e-7); the ranking is made robust with a deterministic id tie-break.

**Parity discipline.** The naive and SIMD paths must produce identical output.
`main.cpp` runs both on the same input and reports: the top-k ranking is identical
(`result diff = 0`), the max per-item score delta (~3e-7), and the speedup
(≈3.6× on the scan alone; lower end-to-end because the top-k sort is shared and
not vectorized). The naive path is kept permanently as the reference.

## 6. The API boundary (`api.hpp`, `bindings.cpp`)

`api.hpp` is the single orchestration layer used by both the native driver and
the WASM bindings — it is glue, not ranking logic:

- `personas()` — the switchable personas (label + per-category interest weights).
- `shared_data()` — the resident item store, built once (a function-local static)
  and reused for every request.
- `recommend(personaId)` — builds the query, assembles the pipeline, runs it,
  returns `{ persona_label, feed, trace }`.
- `to_json(rec)` — hand-written JSON serialization of the feed + trace (the shape
  the frontend consumes). Kept dependency-free.

`bindings.cpp` exposes `recommend` (returns the JSON string), `personaCount`, and
`personaLabel` to JavaScript via embind. There is no engine logic in the
bindings.

## 7. Design principles

- **Readability over cleverness.** The simplest correct version a mid-level
  engineer would write. No template metaprogramming, no obscure tricks.
- **The naive reference is permanent.** The zero-diff parity check is a feature,
  so `dot_scalar` / `recall_naive` are never deleted.
- **Determinism.** Fixed seed; id tie-breaks. Reproducible results and parity.
- **Preserve the trace contract.** `{name, in_count, out_count, latency_us,
  sample_ids}` is fixed; the UI depends on it.
