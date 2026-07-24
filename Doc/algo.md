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

---

## User profile & implicit feedback (v2 · B1)

### What it is

v2 replaces the fixed persona with a **user profile** — a small, persistent model
of what a user likes, built from their behavior. In this block it is three fields
(`profile.ts`):

- `tagWeights` — a weight per interest tag (the growing "interest vector" in tag
  space).
- `clickHistory` — the record of clicked items (tags + timestamp), used later for
  decay and the seen-set.
- `seenItemIds` — the set of items already shown/clicked, for the new-vs-seen mix.

Later blocks turn `tagWeights` into a query vector and run the same recall DAG, so
the profile *is* the query. This block introduces only the model and its
persistence — the feed still runs off the v1 path.

### Implicit feedback

The profile is grown from **implicit feedback** — signals inferred from behavior
(a click) rather than **explicit feedback** a user deliberately gives (a star, a
like). Implicit feedback is far more abundant and is what production recommenders
lean on, at the cost of being noisier and only weakly positive (a click is not a
guaranteed endorsement). v1's persona was closest to explicit input ("I am a
Foodie"); v2's profile is implicit ("you keep opening food posts").

### Why local storage (client-side persistence)

The profile is saved to the browser's `localStorage` and reloaded on return, so a
user's learned interests survive a refresh — **without any account, login, or
backend** (a deliberate scope choice; the engine stays in-browser WASM).

- **Tradeoffs:** per-device and per-browser (not synced across devices), and the
  user can clear it — acceptable here, and honest about what client-only
  persistence buys you.
- **Robustness:** loading is wrapped in try/catch. Storage can be absent, disabled
  (private mode), full, or hold corrupt JSON, and reading a blocked key can throw;
  on any failure we fall back to a **neutral profile** rather than crash.

### Tag → category mapping

The eight cold-start tags fold onto the engine's six item categories, kept in one
place (`TAG_TO_CATEGORY`). Several tags share a category (e.g. News → tech,
Outdoors → travel) because the synthetic data has only six category centroids — a
documented approximation, not a modeling claim.

### The neutral profile (cold-start → warm-up)

With no history the profile is **neutral**: equal weight on every tag. Turned into
a query later, that yields a diverse sampler feed rather than an empty one; the
first few clicks then specialize it. This is the **cold-start** problem and its
simplest reasonable answer: start broad, narrow with evidence.

### Terms an interviewer might probe

User profile / interest vector; implicit vs. explicit feedback; the cold-start
problem; client-side persistence and its tradeoffs; the "profile is the query"
framing that connects behavior to the recall step.
