# algo.md — engine internals, explained for study

A learning / interview-prep companion to the code. Each section explains a
component in plain language: what it does, the key design decisions and WHY, the
tradeoffs, the time/space complexity, and the terms an interviewer might probe.
Read this and you should be able to explain the code out loud. Ordered roughly
foundations-first: the engine core, then the behavior features built on it.

---

## The item store — Structure-of-Arrays (SoA)

### What it does

The item store holds every candidate's embedding vector in memory, ready for the
recall scan. Vectors are stored **Structure-of-Arrays**: one flat `float` buffer
with all vectors concatenated row-major by item (`embeddings`), plus a parallel
array of per-item metadata (`notes`). Item `i`'s vector is the `DIM` floats at
`embeddings[i*DIM …]`; `vector_of(i)` returns a raw pointer to it (no copy).
`DIM = 64`.

### Why SoA, not AoS (the interview question)

- **AoS** (array-of-structs) would be `vector<Item>` where each `Item` owns its own
  vector — simple, but the vectors are scattered across the heap.
- **SoA** puts all the numbers in one contiguous block. The recall kernel walks
  them front-to-back as a single stream, which is (a) **cache-friendly** — each
  cache line is full of useful floats — and (b) **SIMD-friendly** — contiguous
  lanes load straight into vector registers (see the SIMD section).
- Rule of thumb: **SoA when a hot loop sweeps one field over many items** (exactly
  recall); AoS when you touch many fields of one item at a time.
- Tradeoff: SoA is awkward for "give me item i as an object," which is why metadata
  lives in the parallel `notes` array, indexed the same way.

### Complexity

Access is O(1) pointer arithmetic; memory is `count · DIM · 4` bytes (256 B/vector
at DIM=64), so the 3,000-item demo store is under 1 MB.

### Terms an interviewer might probe

Structure-of-Arrays vs. array-of-structs; cache locality / cache lines; how data
layout enables vectorization.

## Synthetic data — category centroids + noise

### What it does

There is no training here, so item vectors are **fabricated** in a way that keeps
recall meaningful. Recipe: each category gets a random **centroid**; each item in
that category is `centroid + small Gaussian noise`, then **unit-normalized**.

### Why it makes recall meaningful

Same-category items land near their shared centroid, so they **cluster** in vector
space; a query near the food centroid therefore recalls food. The content is fake;
the *geometry* (and thus the ranking mechanism) is real — the point of a serving
demo with no training pipeline.

- **Unit-normalizing** makes the dot product equal **cosine similarity**, so
  "similar" = "same direction," and the kernel stays a plain dot product.
- A **fixed PRNG seed** makes the store reproducible — required for the naive/SIMD
  parity check to be trustworthy.

### Terms an interviewer might probe

Embedding space; centroid / cluster; cosine vs. dot; why normalize; deterministic
fixtures; the train/serve split (this is the serve half).

## Recall — vector-similarity candidate generation

### What it does

Recall is the **funnel's widest stage**: cheaply turn a large pool into a few
hundred plausible candidates. It scores **every** item by the dot product of the
query with the item vector, then keeps the **top-k**.

### Design decisions

- **Metric:** dot product — and because vectors are unit-normalized, that *is*
  cosine similarity.
- **Top-k:** score all N, sort descending, keep k, breaking ties by ascending id.
  The **deterministic** tie-break matters: the naive and SIMD paths sum in a
  different order and can differ by ~1e-7, but the id tie-break makes them return
  the *same* ranking. *Rejected for now:* `partial_sort`/heap is better for small k
  but negligibly faster at this N and less clear.
- **Full linear scan:** every item is scored. The production answer to "don't scan
  a million items" is an **approximate-nearest-neighbor index (HNSW)** — a stretch
  goal, not built here.

### Complexity

`O(N·DIM)` scan (the hot path) + `O(N log N)` sort.

### Terms an interviewer might probe

Recall (candidate generation) vs. ranking; cosine similarity; top-k; exact vs.
approximate nearest neighbor (ANN / HNSW / IVF); why recall must be cheap.

## The operator DAG — uniform operators, scheduler, trace

### Everything is an operator

Each stage implements one uniform contract: **take a batch, return a (usually
smaller) batch, and record one trace entry.** No special-cased "recall function" or
"ranking function" — just operators wired into a graph, which is what lets the
pipeline be **extended** (add a node) and **observed** (every node self-reports)
without touching the others.

- The `Batch` is an **array-of-structs** of `Candidate` (id + feature/score columns
  filled progressively). AoS is fine here — the batch is small, transient, and
  never enters the hot kernel; the SoA rule is only for the store.
- Tracing uses the **template-method pattern**: the base `run()` times the stage,
  calls the subclass `transform()`, and appends exactly one `TraceEntry`
  `{name, in, out, latency_us, sample_ids}`. **Why centralize it:** the trace is
  the product, so its shape is produced identically for every stage and can't drift.

### The cascade (the funnel)

Four operators shrink the set stage by stage (demo cardinalities):

- **RecallOp** `3000 → 300` — similarity recall (above).
- **FeatureOp** `300 → 300` — attach ranking features per candidate: category match
  (profile/persona affinity), recency (exponential decay of item age), popularity.
  Enriches; does not filter.
- **ScoreOp** `300 → 50` — a **weighted multi-objective** score. Production rankers
  blend learned per-objective models (pCTR / pLike / pSave); with no trained models
  we blend the features linearly — honest about what it is — then keep top-k.
- **RerankOp** `50 → 12` — **diversity-aware** reordering via greedy **MMR**
  (maximal marginal relevance): choose each next item to maximize
  `λ·score − (1−λ)·redundancy`, with category overlap as redundancy so the page
  isn't twelve of the same thing. This is **exploration/exploitation** in
  miniature — proven relevance vs. variety.

### The DAG scheduler

The scheduler holds the operators as nodes and runs them, threading each stage's
output into the next and collecting the ordered trace. Shua Shua's cascade is a
**linear chain**, so this is a **degenerate DAG** (one path). A general DAG engine
(topological sort, multi-input merging) is **speculative complexity with no
operator that needs it yet**, so it isn't built. "DAG scheduler" is the honest name
because the uniform operator contract is exactly what a real DAG engine schedules.

### Terms an interviewer might probe

Operator/DAG execution model; topological order; template-method; multi-objective
ranking; MMR / diversity; exploration vs. exploitation; observability by design.

## The SIMD recall kernel + naive/SIMD parity check

### Why SIMD, and where

The hot path is the recall dot product — `DIM` multiply-adds per item. That inner
loop is the one place vectorization pays off, so it has two implementations behind
one signature:

- `dot_scalar` — a plain left-to-right accumulate; the **permanent reference**. It
  stays scalar even at `-O2`, because clang won't auto-vectorize a floating-point
  reduction without `-ffast-math` (reassociation changes the result) — so it's an
  honest baseline.
- `dot_simd` — hand-written **NEON** (arm64). A 128-bit register holds **4×
  float32**, so we process 4 elements per iteration into 4 lane accumulators, then
  horizontally sum them. **Why width 4:** that's how many floats fit in one
  register. A **scalar tail loop** covers `dim % 4` (a no-op at DIM=64 but keeps the
  kernel correct for any dimension). Guarded by `__ARM_NEON`; elsewhere (including
  the WASM build) it falls back to the scalar kernel.

### The parity / diff check

Both kernels run on the same input through the same score+sort, so the only
difference is the kernel. The engine then **asserts the top-k ranking is identical
(`diff = 0`)** and reports the max score delta (~1e-7, from summing in a different
order) and the speedup (~3.6× on the scan; less end-to-end because the shared sort
isn't vectorized).

**Why this matters (a strong interview point):** it mirrors a real serving-system
migration — proving an optimization is *faster* **and** changes the result by
nothing. Keeping the naive path forever as the oracle is the feature, not dead code.

### Terms an interviewer might probe

SIMD / vectorization; NEON vs. AVX2; register width / lanes; horizontal reduction;
loop-tail handling; floating-point non-associativity (why we compare ranking, not
bits); parity/diff validation in migrations; Amdahl's law (why end-to-end speedup <
kernel speedup).

---

## Item-based recall — "more like this"

### What it does

The engine's default `recommend(persona)` builds a **query vector** from a
persona's category-centroid blend, then runs the cascade
(Recall → Feature → Score → Rerank). **Item-based recall**
(`recommend_similar(itemId)`) runs the *same* pipeline, but the query is the
**clicked item's own embedding vector** (`store.vector_of(id)`). So instead of
"recommend for this persona," it is "recommend items nearest this item in
embedding space." (In v1 a card click called this and the feed shifted on the
spot. Since v2 · B3 a click instead builds the user profile — see below — and the
feed re-ranks on demand; `recommend_similar` remains in the engine, just no longer
wired to the click.)

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

---

## Cold start & the tag picker (v2 · B2)

### What it does

On a first visit (no onboarded profile in storage) the app shows a one-screen **tag
picker** — the eight interest tags as toggle chips. Selecting is **optional**:
"Continue" seeds the profile from the chosen tags; "Skip" seeds a neutral one.
Either way the profile is marked `onboarded` and persisted, so the picker never
shows again on that browser.

### The cold-start problem

A recommender with **no history** can't personalize — the **cold-start problem**.
Real systems attack it with onboarding (ask a few interests), popularity priors, or
context signals. Here onboarding gives the profile its first signal:

- **Seeded (tags chosen):** those tags get weight, the rest zero — the feed leans
  that way once the profile drives recall (B5).
- **Neutral fallback (skipped):** equal weight on every tag → a **diverse sampler**
  rather than an empty feed. The first clicks then specialize it (B3). This is
  **cold-start → warm-up**, made visible.

### Why `onboarded` is a stored flag

The profile persists from B1, so "have we onboarded?" can't be inferred from "is
storage empty." An explicit `onboarded` boolean (default false — including for any
pre-B2 stored profile) cleanly gates the picker and survives reloads.

### Scope note

B2 seeds and displays the profile; the feed still runs the v1 persona path. Wiring
the profile vector into recall is B5, so onboarding is visible in the profile
readout now and drives the feed later — deliberately incremental.

### Terms an interviewer might probe

Cold-start problem; onboarding / interest elicitation; popularity priors; explicit
onboarding signal vs. implicit click signal; cold-start → warm-up.

---

## Live profile + implicit-feedback accumulation (v2 · B3)

### What it does

Clicking a feed card is now **implicit feedback**: it bumps the weight of the
clicked item's tag(s), appends to click history, and marks the item seen — all in
**real time**. The sidebar's live **profile panel** re-renders immediately, its tag
bars growing (and reordering) as you click. Crucially, **the feed does not move.**

### Real-time profile vs. on-demand feed (the key decision)

Two independent update rates:

- The **profile updates on every click** — instant, legible feedback ("my action
  registered"), and the bars visibly grow.
- The **feed re-ranks only on demand** (the "Refresh recommendations" button, B6),
  never on a click.

**Why split them:** jumping the feed on every click destroys cause-and-effect (you
can't see which click changed what) and is jarring; freezing everything until a
manual refresh loses the "it's learning" signal. Splitting gives both — live
profile growth *and* a deliberate reveal. It also mirrors production systems, where
behavior is **logged in real time** but recommendations are **recomputed in
batches**.

### Immutability

`recordClick` returns a *new* profile object (copied weights, history, seen-set)
rather than mutating in place, so React sees a changed reference and re-renders the
panel; the change is also persisted (B1's save effect).

### The tag→category coupling

An item only carries a category, and several tags fold onto one category, so
clicking (e.g.) a tech item bumps every tag mapped to tech (Tech *and* News) — a
consequence of the coarse synthetic category space, not a modeling choice.

### Terms an interviewer might probe

Implicit feedback / behavioral signals; real-time logging vs. batch recomputation;
online vs. batch updates; why immediate feed mutation harms legibility; immutable
state updates in a UI.

---

## Interest decay (v2 · B4)

### What it does

Interests fade so the profile can drift toward what the user cares about *now*.
`decayProfile` multiplies every tag weight by `DECAY_FACTOR` (0.7) on each
**refresh**. Because new clicks enter at full weight (B3) while old weights keep
getting multiplied down, tags you stop feeding shrink and recent clicks come to
dominate — "recent clicks weigh more than old."

### Half-life vs. per-refresh (the decision)

- **Time-based half-life** — `weight = base · exp(-λ·Δt)`: a click's influence
  decays continuously with wall-clock age. The "real" model.
- **Per-refresh multiplicative** — on each refresh, `weight *= factor`: event-based,
  decay happens when the user acts.

We chose **per-refresh**. In a click-driven demo almost no wall-clock time passes,
so time-based decay would look like nothing ever fades — invisible exactly where we
want to show it. Event-based decay ties the fade to a user action, keeping cause and
effect legible, and it's a single explainable parameter, no λ to tune (§9: don't
over-engineer decay).

### Why the effect is recency, not shrinking bars

A uniform multiply scales every weight equally, so on its own it doesn't change the
*relative* bars. The visible effect is the **asymmetry**: refreshes decay old
weights while fresh clicks enter at full strength. Click topic A, then refresh while
clicking topic B, and B overtakes A even at equal click counts — that overtaking is
decay made visible.

### Guard (§6)

If every weight decays to ~0 (many refreshes, no clicks), `decayProfile` falls back
to the neutral profile, so the recall query built from it (B5) is never a zero/NaN
vector.

### Trigger note

The refresh event in this block is switching persona (the only feed re-run today);
B6 moves the decay trigger onto the dedicated "Refresh recommendations" button.

### Terms an interviewer might probe

Interest decay / recency; exponential half-life vs. event-based decay; why recency
matters; single-parameter simplicity; guarding against a degenerate profile vector.

---

## Profile vector = recall query (v2 · B5)

This is where the profile stops being a sidebar decoration and becomes the thing
that drives recall. The whole v2 thesis in one line: **recommendation = use the
profile vector as the recall query, then run the existing DAG.**

### The pipeline of translations

```
tagWeights (8)  ──►  categoryWeights (6)  ──►  profile vector (DIM=64)  ──►  RecallOp query
   profile.ts           profile.ts                   api.hpp                  (unchanged DAG)
```

Three deliberate hops, each in the language that owns that data:

**1. tags → categories (TypeScript).** The profile stores 8 tag weights; the engine
knows only 6 item categories. `categoryWeights` folds one onto the other via the
single `TAG_TO_CATEGORY` map — the *only* tag→category translation in the app:

```ts
// web/src/profile.ts — categoryWeights()
const w = new Array<number>(CATEGORY_ORDER.length).fill(0);
for (const tag of TAGS) {
  const idx = (CATEGORY_ORDER as readonly string[]).indexOf(TAG_TO_CATEGORY[tag]);
  if (idx >= 0) w[idx] += profile.tagWeights[tag] ?? 0;
}
return w;
```

`CATEGORY_ORDER` = `["food","fashion","travel","tech","fitness","beauty"]` is pinned
to match C++'s `CATEGORY_NAMES` (`src/api.hpp`) — the one ordering the two languages
must agree on, because the C++ side indexes centroids positionally.

**2. weights → vector (C++).** The vector-space math lives entirely in C++ so it
can't drift from the persona path. `recommend_from_profile` reuses the exact
`make_query` personas use — the query is `normalize(Σ wᶜ · centroidᶜ)`:

```cpp
// src/api.hpp — make_query() (shared by personas and the profile)
for (std::size_t c = 0; c < category_weights.size(); ++c)
  for (std::size_t d = 0; d < ItemStore::DIM; ++d)
    query[d] += category_weights[c] * centroids[c][d];
normalize(query.data(), ItemStore::DIM);
```

```cpp
// src/api.hpp — the v2 entry point
inline Recommendation recommend_from_profile(std::vector<float> category_weights) {
  // ... §6 guard: wrong-size or all-zero weights → uniform (neutral) blend ...
  return run_recommendation(make_query(category_weights, shared_data().centroids),
                            category_weights, "For you");
}
```

So `recommend_from_profile` and `recommend` (persona) differ in exactly one way —
where the weights come from. Everything downstream (`run_recommendation` and the
Recall→Feature→Score→Rerank DAG) is the same code.

**3. the boundary (embind).** JS hands the 6 weights across as a CSV string — the
simplest robust crossing for a fixed, tiny float vector (no `register_vector` or
manual `.delete()`):

```cpp
// src/bindings.cpp
static std::string recommend_from_profile_json(const std::string& weights_csv) {
  // split on ',', std::stof each field, then recommend_from_profile(...)
}
emscripten::function("recommendFromProfile", &recommend_from_profile_json);
```

```ts
// web/src/engine.ts
export async function recommendFromProfile(categoryWeights: number[]) {
  const engine = await loadEngine();
  return JSON.parse(engine.recommendFromProfile(categoryWeights.join(","))) as Recommendation;
}
```

### When the feed runs (and when it doesn't)

`App.runFeed(profile)` calls `recommendFromProfile(categoryWeights(profile))`. It's a
plain function, *not* an effect keyed on the profile, because the feed must re-run
only on explicit events:

- **mount**, for a returning onboarded user (`useEffect([], …)`);
- **onboarding finish**, for a new user (`finishOnboarding` → seed → `runFeed`);
- **the refresh button** — added in B6.

It must NOT re-run on clicks — that's B3's real-time-profile / on-demand-feed split,
which is why `handleCardClick` only does `setProfile(recordClick(...))`.

### What this block removed

v1's persona switcher is gone: the feed is the profile's now, so a fixed picker no
longer fits the model. `personas()` still exists in `api.hpp` (and `recommend` /
`recommendSimilar` remain bound) but the UI no longer calls them. B4's decay trigger
rode on persona switching, so decay is dormant this block and gets its real trigger —
the refresh button — in B6.

### Rebuild step

Because this changes C++, the WASM must be rebuilt: `scripts/build-wasm.sh` →
`web/public/shuashua.js` (single-file, wasm embedded, loaded via a `<script>` tag).
Stale WASM throws "recommendFromProfile is not a function."

### Terms an interviewer might probe

Query/embedding vector; a user profile as a point in item space; centroid blend;
keeping vector math in one place to avoid serving skew; the FFI boundary (embind) and
marshalling; why recommendation reduces to "profile → query → DAG."

---

## Refresh + the new/seen mix — exploration/exploitation (v2 · B6)

### The refresh button

The feed re-ranks only when the user presses **"Refresh recommendations"** — the
on-demand half of B3's real-time-profile / on-demand-feed split. `App.handleRefresh`
does two things:

```ts
// web/src/App.tsx
const handleRefresh = () => {
  const decayed = decayProfile(profile);  // B4's decay is triggered HERE now
  setProfile(decayed);
  runFeed(decayed);                        // re-rank against the aged profile
};
```

A refresh both **ages** the profile (interests you stopped feeding fade — B4's
trigger, dormant since B5, lives here) and **recomputes** the feed. That's the
"batch recompute"; clicks between refreshes only grow the profile.

### The problem the mix solves

If a refresh just re-ran recall against the profile, it would return **the items the
user already clicked** — those score highest *because* they were clicked (they match
the profile). The feed would never move on. Real systems avoid this with the
**exploration/exploitation** tradeoff: mostly show new things (explore), keep a few
proven favorites (exploit).

### MixOp — the quota, guaranteed

`MixOp` (`src/mix_op.hpp`) is a new final operator. It splits the ranked pool into
**new** (id ∉ seen) and **seen** (id ∈ seen), then fills the page mostly from new
with a small seen quota set by `new_ratio` (default 80 → ~2–3 seen of 12):

```cpp
// src/mix_op.hpp — MixOp::transform (core)
std::size_t new_target = llround(page_size_ * new_ratio_ / 100.0);
std::size_t take_new  = std::min(new_target, new_items.size());
std::size_t take_seen = std::min(page_size_ - new_target, seen_items.size());
// ... backfill any shortfall from the other pool (prefer new) ...
// emit new first, then the reserved seen
```

**Why a separate operator, placed last** (`src/api.hpp` `run_recommendation`):
RerankOp's job is category *diversity* (MMR); MixOp's is *recency-of-exposure*
balance — one purpose each. It runs LAST so the seen quota is guaranteed; a
mix-before-rerank order would let RerankOp's diversity cut drop the reserved seen.
The profile path grows a fifth stage, but only when there is a seen set to mix:

```cpp
// src/api.hpp — run_recommendation
if (!seen_ids.empty() && new_ratio < 100) {
  pipeline.add(std::make_unique<RerankOp>(data.store, kRerankPool, kRerankLambda)); // 50 -> 24 diverse
  pipeline.add(std::make_unique<MixOp>(std::move(seen_ids), kPageSize, new_ratio)); // 24 -> 12 new/seen
} else {
  pipeline.add(std::make_unique<RerankOp>(data.store, kPageSize, kRerankLambda));   // 50 -> 12
}
```

So the DAG trace shows the difference: a first, nothing-clicked feed traces **4
ops**; a refresh after clicking traces **5** — MixOp appears, making the
exploration/exploitation step observable (the trace is the product).

### The boundary

The seen set crosses to C++ the same CSV way the weights do —
`recommendFromProfile(categoryWeights.join(","), [...seenItemIds].join(","), NEW_RATIO)`
(`web/src/engine.ts`) — parsed back into a `std::vector<std::uint32_t>` in
`bindings.cpp`.

### Terms an interviewer might probe

Exploration vs. exploitation; the staleness / "filter bubble" failure mode; why
already-seen items score high and must be capped; reserved-quota page assembly;
single-purpose operators; batch recompute vs. real-time signal.
