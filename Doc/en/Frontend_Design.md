# Frontend Design

The frontend (`web/`) is a Vite + React + TypeScript single-page app that renders
the WASM engine's output as a Xiaohongshu-style feed with a live DAG trace panel.
It is pure presentation — there is no ranking logic on the JS side. What it *does*
own is the **user profile** (v2): it builds the profile from cold-start tags and
clicks, and hands the engine the per-category weights to recommend from.

## 1. Design language

The visual design follows the Xiaohongshu (Little Red Book) web app, adapted from
the open-source XiaoShiLiu reference for proportions (components were written
fresh, not copied):

- **Palette** (CSS variables): a warm-red accent `#ff2442` used sparingly (logo,
  key actions such as Refresh, out-counts), with a neutral text/background/border
  token set.
- **Layout:** a fixed left sidebar (~232px) plus a main content area — the web
  Xiaohongshu shape, not a mobile bottom-tab layout.
- **Feed:** a responsive masonry waterfall of borderless, rounded cards.
- **Dark mode:** a full second theme via `[data-theme="dark"]`, toggleable.

## 2. Layout and components

```
┌────────────┬───────────────────────────────────────────────┐
│  Sidebar   │  Explore · For you            [↻ Refresh]      │
│  ┌──────┐  │  ┌─────────────────────────────────────────┐  │
│  │ 刷    │  │  │ DAG pipeline trace (collapsible funnel)  │  │
│  │ brand │  │  └─────────────────────────────────────────┘  │
│  ├──────┤  │  ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐            │
│  │profile│  │  │card│ │card│ │card│ │card│ │card│  waterfall │
│  │ panel │  │  └────┘ └────┘ └────┘ └────┘ └────┘  (2–5 cols)│
│  ├──────┤  │  ...                                            │
│  │theme  │  │                                                │
│  └──────┘  │                                                │
└────────────┴───────────────────────────────────────────────┘
```

| Component | File | Responsibility |
|---|---|---|
| `App` | `src/App.tsx` | Owns state: the user profile (loaded, persisted, seeded, decayed), the current recommendation, the theme. Runs the feed only on **explicit events** — the initial load and the Refresh button — never on every click. Shows the cold-start picker until the profile is onboarded. |
| `ColdStart` | `src/components/ColdStart.tsx` | First-visit tag picker (optional). Selected tags seed the initial profile; skipping falls back to a neutral (diverse) one. Either way the profile is marked onboarded. |
| `Sidebar` | `src/components/Sidebar.tsx` | Brand, the **live profile panel** (this replaces v1's persona switcher), the light/dark toggle. |
| `ProfilePanel` | `src/components/ProfilePanel.tsx` | The profile's tag weights as bars, normalized to the current max; grows and reorders live as clicks bump weights. |
| `Feed` | `src/components/Feed.tsx` | `react-masonry-css` waterfall; responsive column counts; loads the cover manifest. |
| `NoteCard` | `src/components/NoteCard.tsx` | One card: cover → 2-line title → "why" caption → author + likes. A card click is implicit feedback (bumps the item's category tags). |
| `TracePanel` | `src/components/TracePanel.tsx` | Collapsible DAG funnel: per-stage in→out, latency, optional detail, sample ids; names the profile that drove the run; replays a staggered reveal + flash when the numbers change. |

## 3. Engine integration (`engine.ts`)

The engine ships as a single-file, classic script (`public/shuashua.js`, wasm
embedded) that attaches a global `ShuaShua()` factory. `engine.ts` loads it with
a `<script>` tag and exposes typed wrappers:

- `recommendFromProfile(categoryWeights, seenIds, newRatio)` → `Recommendation`
  — **the live path.** The profile's per-category weights and the seen-item set
  cross the boundary as **CSV strings** (a fixed, tiny float vector — the simplest
  robust embind crossing, no `register_vector` ceremony); C++ builds the query
  vector and runs the DAG.
- `recommend(personaId)` / `recommendSimilar(itemId)` → `Recommendation` — the v1
  persona and item-similarity paths. Retained in the engine as alternate query
  sources; **not called by the current UI.**
- `getPersonas()` → `Persona[]`.

All three return the same JSON shape (`{ persona, feed[], trace[] }`).

**Why a `<script>` tag, not an ESM `import`:** the engine lives in `public/`, and
Vite's dev server refuses to ESM-import `public/` assets (they may only be
referenced via HTML tags). A single-file classic build + script tag works
identically in dev, preview, and production. The module is loaded once and cached
(so the C++ store is built once per session).

## 4. The user profile (v2 · `profile.ts`)

v2 replaces v1's fixed personas with a **behavior-driven profile** — a living,
decaying interest model. `profile.ts` is the whole model; the vector-space math
stays in C++.

**State.**

```ts
interface Profile {
  tagWeights: Record<string, number>; // accumulated, decayed weight per tag
  clickHistory: ClickRecord[];         // for the seen set + history
  seenItemIds: Set<number>;            // supports the new/seen mix on refresh
  onboarded: boolean;                  // has the cold-start picker been passed
}
```

**Tags → categories (one place).** The eight cold-start tags (Travel, Food, Tech,
News, Art, Sports, Literature, Outdoors) fold onto the engine's six item
categories via `TAG_TO_CATEGORY` (e.g. News → tech, Art → fashion, Sports →
fitness, Literature → beauty, Outdoors → travel). The tag set is intentionally
larger than the category set — a documented approximation, since the synthetic
data has no separate "news"/"literature" item vectors. `categoryWeights(profile)`
collapses the eight tag weights into six per-category weights in `CATEGORY_ORDER`
(`food, fashion, travel, tech, fitness, beauty`) — the **one** place the two
languages must agree on ordering, because C++ indexes centroids by it. The
weighted-centroid blend itself happens in C++ (`api.hpp make_query`), so the
profile query is built exactly like a persona query and cannot drift.

**Cold start (`ColdStart`, B2).** An optional picker seeds the initial profile:
selected tags get weight 1; **skipping** falls back to a *neutral* profile (every
tag equal), so the first feed is a diverse sampler rather than empty — and the
first few clicks quickly specialize it (cold-start → warm-up).

**Implicit feedback (`recordClick`, B3).** A card click bumps the weight (+1) of
every tag that maps to the clicked item's category, appends to `clickHistory`, and
marks the item seen. It returns a **new** profile (immutable update), so React
re-renders the live bars immediately.

**Interest decay (`decayProfile`, B4).** Decay is **per-refresh multiplicative**:
each refresh multiplies every tag weight by `DECAY_FACTOR = 0.5`. We rejected
time-based `exp(-λΔt)` decay because in a click-driven demo almost no wall-clock
time passes, so it would look like nothing ever fades; event-based decay makes the
fade happen exactly when the user acts. New clicks enter at full weight, so a tag
you keep feeding stays high while one you stop feeding halves each refresh —
recent interest outweighs stale. A guard resets to neutral if everything decays to
~0, so the recall query is never a zero/NaN vector.

**Real-time vs. on-demand.** The profile (and the panel) update on **every**
click; the **feed** re-ranks only on **Refresh** — which is also when a decay step
is applied. This split is the core v2 UX decision (see
[Architecture §6](Architecture.md#6-key-architectural-decisions)).

**The new/seen mix on refresh (exploration/exploitation).** `NEW_RATIO = 80` is
passed to the engine: a refreshed page is ~80% **new** (unseen) items, reserving a
small ~2–3-of-12 quota for top-scoring already-seen favorites, on top of the
guaranteed exploration floor from outside the dominant category. The mix logic
itself lives in the engine's `MixOp` (see [Core_Design](Core_Design.md)); the
frontend only supplies the seen set and the ratio.

**Persistence.** The profile and click history are saved to `localStorage` on
every change (`seenItemIds` is stored as an array, since a `Set` isn't
JSON-serializable, and rehydrated on load). Loading is wrapped in try/catch:
missing, disabled, or corrupt storage falls back to a fresh neutral profile rather
than crashing.

## 5. Presentation layer

The engine has no titles, authors, cover images, or like counts — those are
frontend concerns.

- **`presentation.ts`** synthesizes per-note content deterministically from the
  note id (same card → same content on refresh): title (from a per-category
  pool), author + avatar, like count (from `popularity`). The **"why recommended"
  line is derived from the engine's real feature values** — it surfaces the
  strongest ranking signal among `category_match` / `recency` / `popularity`,
  weighted as `ScoreOp` weights them, so the caption reflects what actually moved
  the item up.
- **`covers.ts`** loads the local cover manifest (`/covers/manifest.json`) once
  and picks a cover per note deterministically by id. Each cover shows the
  required "Photo by … on Unsplash" attribution. Missing image / category →
  graceful fallback to a gradient + category emoji.

Cover images are fetched at **build time** and committed; the runtime makes zero
Unsplash API calls (see [Operations](Operations.md)).

## 6. Theming

- Themes are CSS-variable sets: light in `:root`, dark under `[data-theme="dark"]`.
- A tiny inline script in `index.html` resolves the theme **before first paint**
  (order: `?theme=` query param → saved choice → OS preference) to avoid a flash.
- `App` reads the resolved theme, and the toggle persists the choice to
  `localStorage`. The `?theme=` param also makes the theme shareable/screenshotable.

## 7. The trace visualization

The DAG trace panel is the visible half of the project — "the trace is the
product." It renders the pipeline stages as a left-to-right funnel showing
`in → out`, latency, an optional `detail` line, and sample ids, next to a short
summary of **which profile drove this run** ("driven by …"). On the profile path
there are **five** stages (the extra one is `MixOp`, whose detail reads e.g.
`"10 exploit · 2 explore"`). A staggered CSS reveal — plus a brief flash — replays
whenever the counts change, tying the Refresh press to the recomputation. It is
collapsible so the feed stays the focus.

Note on latency: the browser only reports sharp sub-millisecond timings when the
page is cross-origin-isolated (COOP/COEP), which the app requests. When it isn't
(e.g. a static host that can't send those headers, or a headless screenshot with a
virtual time budget), the panel says so — a `0.0µs` reads as "timer clamped," not
"the C++ was instant." A real, isolated browser shows real microseconds.

## 8. Build

Standard Vite: `npm install`, `npm run dev` (dev server), `npm run build`
(type-check + production bundle to `dist/`), `npm run preview`. The engine
(`public/shuashua.js`) and covers (`public/covers/`) are static assets copied
into the build. See [Operations](Operations.md).
