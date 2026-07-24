# algo.md — engine internals, explained for study

A learning / interview-prep companion to the code. Each section explains a
component in plain language: what it does, the key design decisions and WHY, the
tradeoffs, the time/space complexity, and the terms an interviewer might probe.
Read this and you should be able to explain the code out loud. (Sections are
added as components are studied; this is not exhaustive yet.)

---

## Item-based recall — "more like this"

### What it does

The engine's default `recommend(persona)` builds a **query vector** from a
persona's category-centroid blend, then runs the cascade
(Recall → Feature → Score → Rerank). **Item-based recall**
(`recommend_similar(itemId)`) runs the *same* pipeline, but the query is the
**clicked item's own embedding vector** (`store.vector_of(id)`). So instead of
"recommend for this persona," it is "recommend items nearest this item in
embedding space." In the UI, clicking a card calls it and the feed shifts toward
that item's neighborhood.

### How it models "user clicked X → show similar to X"

A click is **implicit positive feedback**. A simple, strong response is
**item-to-item recall**: treat the clicked item's representation as the query and
retrieve its nearest neighbors. This is the content-based cousin of item-item
collaborative filtering ("people who liked X also liked…"), except similarity
here is computed **directly from item embeddings** (vector similarity) rather than
from co-engagement. It needs no user history and no model retraining — just the
item store and the same recall kernel.

### Key design decision — reuse the exact same pipeline

`recommend()` and `recommend_similar()` both delegate to
`run_recommendation(query, category_weights, label)`; the **only** difference is
where the query comes from (a persona's centroid blend vs. an item's own vector).

- **WHY:** the ranking logic is identical regardless of the query's origin — a
  "query" is just a point in embedding space. Unifying them keeps one code path
  and prevents drift. *Rejected alternative:* a separate implementation for
  item-based recall — needless duplication and a second thing to keep correct.
- **Category weights:** for item-based recall we set weight `1.0` on the clicked
  item's own category (the implicit "you're into this kind of thing" signal), so
  `FeatureOp`'s `category_match` rewards same-category items. The clicked item
  itself ranks first (a unit vector's similarity to itself is `1.0`), then its
  neighbors follow.

### Determinism — a feature, not a bug

Same query → same result, every time. `recommend_similar(100)` always returns the
same feed.

- **WHY this is desirable in serving:** reproducibility (A/B tests and debugging
  need stable output for a fixed input), cacheability, and explainability.
  Personalization and novelty come from the **input** changing — a different
  clicked item, persona, or context — not from injecting randomness into the
  ranker. A serving engine is a pure function of `(query, store)`; with the store
  fixed, behavior is fully determined by the query.
- **This demo's data:** the synthetic vectors encode only **category** (each note
  = category centroid + small noise). So "similar to a food item" returns other
  **food** items (the same cluster) — a real shift in the feed (different items,
  different trace `sample_ids`), but not sub-category structure like "meat vs.
  salad," because the data has none. With learned embeddings the identical
  mechanism would surface genuine sub-topic neighborhoods.

### Complexity

Identical to persona recall:

- **Time:** recall scans all `N` items, each a `DIM`-dimensional dot product →
  `O(N·DIM)`; then top-k via a full sort → `O(N log N)`. For the demo (`N=3000`,
  `DIM=64`) this is well under a millisecond natively.
- **Space:** `O(N)` for the scored candidate list.
- Sublinear recall (an approximate-nearest-neighbor index such as HNSW) is the
  production answer for large `N`, and is a stretch goal here.

### Terms an interviewer might probe

- **Item-to-item / item-based recall**; content-based vs. collaborative filtering.
- **Embedding / vector similarity** — cosine vs. dot product (here vectors are
  unit-normalized, so the two are equal).
- **Implicit feedback** (a click) vs. explicit feedback (a rating).
- **Deterministic serving** — why fixed input → fixed output is a property you
  want, and where novelty actually comes from.
- **Nearest-neighbor search** and ANN indexes (HNSW) for scaling recall.
- The unifying framing: "a query is just a point in embedding space," which makes
  persona recall and item recall the same operation with different query sources.
