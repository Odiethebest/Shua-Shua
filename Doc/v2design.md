# Shua Shua v2 — Behavior-Driven User Profile (Design Doc)

> Refactor from static "personas" to a dynamic **user profile** that is built at
> cold start and continuously reshaped by user behavior. This document is both the
> build spec for the implementation and the study/interview reference for how the
> feature maps onto real recommendation-system concepts.

---

## 1. Why this refactor

The v1 demo let a user pick a fixed persona (Foodie / Traveler / ...) and served a
deterministic feed for it. That correctly demonstrated *per-user* recommendation,
but it could not show the single most compelling thing a recommender does: **learn
from behavior**. A persona is a label you pick; a profile is something your actions
build.

v2 models the full lifecycle of a real recommender:

| Real-system concept | Where it shows up in v2 |
|---|---|
| Cold start / onboarding | Tag-selection screen on first visit |
| User profile / interest vector | A weighted vector accumulated from tags + clicks |
| Implicit feedback | Each card click bumps that content's tags |
| Interest decay / recency | Recent clicks weigh more than old ones |
| Batch vs. real-time update | Profile updates live; feed re-ranks on demand |
| Exploration vs. exploitation | "Mostly new + some strongly-related seen" mix |
| Personalization over a candidate pool | The existing Recall→Feature→Score→Rerank DAG |

The engine (SoA store, operator DAG, SIMD recall, diff check) is unchanged in
spirit. What changes is **how the query vector is built**: v1 built it from a fixed
persona; v2 builds it from a living, decaying profile.

---

## 2. The core mental model

Everything hinges on one object: the **profile vector** — a single DIM-length
vector in the same space as item vectors, representing "what this user likes right
now." Recommendation is just: *use the profile vector as the recall query, run the
DAG, return the feed.*

```
tags chosen at cold start  ─┐
                            ├─►  profile vector  ──►  RecallOp query  ──►  DAG  ──►  feed
clicks (decayed over time) ─┘
```

Two independent update rates (this is the key design decision):

- **Profile updates in REAL TIME.** Every click immediately changes the profile and
  its visible tag weights. The user sees their interests grow as they act.
- **The FEED re-ranks ON DEMAND.** The feed does not jump on every click. It updates
  only when the user presses **"Refresh recommendations."**

> **Why split them (design rationale — worth stating in an interview):** jumping
> the feed on every click destroys the sense of cause and effect (you can't tell
> which click did what) and is jarring. Freezing everything until a manual refresh
> loses the satisfying "my actions are being recorded" feedback. Splitting the two
> gives both: live, legible profile growth (instant feedback) plus a deliberate
> "reveal" when the user asks for it. It also mirrors production systems where
> behavior is logged in real time but recommendations are recomputed in batches.

---

## 3. User flow

1. **Cold start.** First visit shows a tag picker: Travel, Food, Tech, News, Art,
   Sports, Literature, Outdoors. Selecting is optional — the user may skip straight
   in (see §6 for how we handle the empty case). Chosen tags seed the initial
   profile.
2. **Initial feed.** Home shows a feed produced by the seeded profile, with the DAG
   trace above it showing how it was computed.
3. **Browse & click.** As the user clicks cards, the **profile panel updates live**
   — the clicked content's tags gain weight, the tag bars grow, recent clicks
   weigh more. The **feed does not move yet.**
4. **Reveal.** The user presses **"Refresh recommendations."** The feed re-runs the
   engine against the current profile and re-ranks. The DAG trace updates to reflect
   this run.
5. **Persist.** Profile + click history are saved to browser local storage, so
   returning later resumes the same learned profile.

---

## 4. Data & state model

### 4.1 Tags and categories

v1 already has categories on items (food/travel/tech/...). v2's tag set should map
onto the same category space so tag weights translate directly into vector space.
If the tag set is larger than the category set, define a fixed tag→category (or
tag→centroid) mapping. Keep this mapping in one place.

### 4.2 The profile (frontend state)

```
Profile {
  tagWeights: { [tag]: number }   // accumulated, decayed weight per tag
  clickHistory: Array<{ itemId, tags, timestamp }>   // for decay + "seen" set
  seenItemIds: Set<itemId>        // to support the new/seen mix on refresh
}
```

- `tagWeights` is what the UI shows as growing bars and what gets turned into the
  profile vector.
- `clickHistory` lets us recompute decayed weights and know what's been seen.
- `seenItemIds` supports the "mostly new + some strongly-related seen" refresh rule.

### 4.3 Building the profile vector

The profile vector fed to the engine is a weighted sum of the tag/category
centroid vectors:

```
profileVector = normalize( Σ_tag  decayedWeight(tag) * centroid(tag) )
```

Where the centroids are the same ones the synthetic-data generator used (so the
profile lives in the item vector space and recall is meaningful).

### 4.4 Interest decay (you chose: recency-weighted)

Each click's contribution decays with age. Use a simple, explainable rule — an
exponential half-life is standard and easy to justify:

```
weight_of_click(t_now) = base * exp(-λ * (t_now - t_click))
```

Or, simpler and just as demoable, a per-refresh multiplicative decay: on each
refresh, multiply all existing tag weights by a decay factor (e.g. 0.9) before
adding the new clicks since last refresh. Pick ONE and document it in algo.md.

> **Design note (my guardrail on your choice):** true time-based `exp(-λΔt)` is
> more "real," but in a click-driven demo where time barely passes, it will look
> like nothing decays. The **per-refresh decay** variant is recommended for the
> demo because decay becomes visible exactly when the user acts (refreshes), which
> is what you want to show. algo.md should explain both and why we chose the
> event-based one for legibility.

---

## 5. Where the engine changes

The engine's job is unchanged: take a query vector, run Recall→Feature→Score→Rerank,
return feed + trace. Two additions:

### 5.1 A profile-vector entry point

v1 built the query from a persona id inside C++. v2 should let the **frontend supply
the query vector** (the profile vector it computed) — or supply tag weights and let
C++ build the vector. Either is fine; prefer whichever keeps the vector-space math
in one language to avoid drift. Add an embind entry like:

```cpp
// query is the DIM-length profile vector (or tag weights the engine turns into one)
std::string recommend_from_profile(const std::vector<float>& query,
                                    const std::vector<uint32_t>& excludeSeen,
                                    int newRatio /* 0..100 */);
```

Rebuild WASM after this (`scripts/build-wasm.sh`).

### 5.2 The new/seen mix on refresh (you chose: mostly new + some strong-related seen)

RerankOp (or a thin post-step) must assemble the final page from two pools:
- **New pool:** candidates whose ids are not in `seenItemIds`.
- **Seen pool:** candidates that ARE in `seenItemIds` but score very highly
  (strongly related to the current profile).

Fill the page mostly from the new pool, reserving a small quota (e.g. 2–3 of 12)
for top-scoring seen items. Make the ratio a parameter so it's tunable and
explainable.

> **Design note (my guardrail):** this "mix" is really the
> **exploration/exploitation tradeoff** in miniature — show new things (explore)
> but keep a few proven favorites (exploit). Name it that way in algo.md; it's a
> concept interviewers probe and you'll sound fluent.

---

## 6. Edge cases to specify (don't let these be discovered at runtime)

- **Empty cold start (user skips all tags).** You chose "optional." So define the
  fallback: with no tags, seed a **uniform/neutral profile** (equal weight to all
  categories) so the first feed is a diverse sampler rather than empty. The first
  few clicks then quickly specialize it — which is itself a nice demonstration of
  cold-start → warm-up.
- **All weights decayed to ~0.** Guard against a zero/NaN profile vector; fall back
  to the neutral profile.
- **Local storage absent/corrupt.** Wrap load in try/catch; on failure, start fresh
  (this matches the artifact storage guidance — accessing a missing key throws).
- **Seen pool empty on first refresh.** Fine — page fills entirely from new pool.

---

## 7. The DAG trace panel (make it more legible — your ask)

v1's trace showed fixed counts and 0.0µs latency, which read as "dead." v2 should:

- Show the **profile** driving this run (the current top tag weights) next to the
  trace, so it's clear the feed came from *this* profile.
- Keep per-op in→out counts and sample ids. If you later make counts vary
  (optional), they'll animate naturally.
- Fix latency display (COOP/COEP headers for real `performance.now()` precision) so
  microsecond timings show instead of 0.0µs — otherwise the "C++ is fast" story is
  invisible.
- On refresh, briefly highlight that the trace re-ran (a subtle animation), tying
  the button press to the recomputation.

---

## 8. Build plan (ordered, one block at a time, commit + algo.md each)

Follow the same discipline as before: do ONE block, build, run, update
`docs/algo.md`, commit that block alone, then stop for review.

- **B1 — Profile state + local storage.** Introduce the Profile model, tag weights,
  click history, persistence. No engine change yet; feed can still use a
  profile-vector built on the frontend and passed to the existing recommend path.
  algo.md: user profile, implicit feedback, why local storage.
- **B2 — Cold-start tag picker.** Onboarding screen, optional selection, neutral
  fallback, seeds initial profile. algo.md: cold start, neutral profile warm-up.
- **B3 — Live profile panel + click accumulation.** Clicking a card bumps that
  item's tags in real time; tag bars grow; feed does NOT move yet. algo.md:
  real-time vs. batch update rationale.
- **B4 — Interest decay.** Add the chosen decay rule; weights shrink as they age /
  per refresh. algo.md: interest decay, half-life vs. per-refresh, why we chose ours.
- **B5 — Engine: recommend_from_profile + rebuild WASM.** Frontend passes the
  profile vector; engine runs the DAG against it. algo.md: profile vector = recall
  query, vector-space math.
- **B6 — Refresh button + new/seen mix.** The manual "Refresh recommendations"
  button re-ranks the feed; RerankOp assembles mostly-new + few-strong-seen.
  algo.md: exploration/exploitation, the mix ratio.
- **B7 — Trace panel polish + latency fix.** Show driving profile, fix COOP/COEP
  latency, add re-run highlight. algo.md: observability, why latency was 0.

Each block is independently runnable and demoable. Do not pull a later block's
complexity into an earlier one.

---

## 9. What NOT to do (scope guard)

- Do not add a real login/account system — local storage is the whole persistence
  story.
- Do not add a backend or database — the engine stays in-browser WASM.
- Do not train embeddings — centroids remain synthetic; the profile is a weighted
  combination of them.
- Do not make the feed auto-jump on every click — the manual refresh is deliberate.
- Do not over-engineer decay — one simple, explainable rule beats a tunable
  multi-parameter model for this demo.

---

## 10. Interview framing (why this doc doubles as a study aid)

When asked about this project, the strong narrative is:

> "It's an in-browser C++ recommendation serving engine. v2 models the full
> recommender lifecycle: cold-start onboarding builds an initial user profile;
> implicit feedback from clicks reshapes a decaying interest vector in real time;
> and a manual refresh re-ranks the feed against the current profile, balancing new
> content against proven favorites — exploration vs. exploitation. The heavy lifting
> — vector recall over a Structure-of-Arrays store — is a hand-vectorized C++ kernel
> compiled to WebAssembly, with a naive reference path kept for a parity/diff check."

Every italicized term above is a real concept an interviewer can probe, and each is
explained in `docs/algo.md`. Read algo.md as you build each block, so the code and
the vocabulary land together.