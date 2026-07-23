# Frontend Design

The frontend (`web/`) is a Vite + React + TypeScript single-page app that renders
the WASM engine's output as a Xiaohongshu-style feed with a live DAG trace panel.
It is pure presentation — there is no ranking logic on the JS side.

## 1. Design language

The visual design follows the Xiaohongshu (Little Red Book) web app, adapted from
the open-source XiaoShiLiu reference for proportions (components were written
fresh, not copied):

- **Palette** (CSS variables): warm-red accent `#ff2442` used sparingly (logo,
  active persona, out-counts), with a neutral text/background/border token set.
- **Layout:** a fixed left sidebar (~232px) plus a main content area — the web
  Xiaohongshu shape, not a mobile bottom-tab layout.
- **Feed:** a responsive masonry waterfall of borderless, rounded cards.
- **Dark mode:** a full second theme via `[data-theme="dark"]`, toggleable.

## 2. Layout and components

```
┌────────────┬───────────────────────────────────────────────┐
│  Sidebar   │  Explore  ·  <persona>                         │
│  ┌──────┐  │  ┌─────────────────────────────────────────┐  │
│  │ 刷    │  │  │ DAG pipeline trace (collapsible funnel)  │  │
│  │ brand │  │  └─────────────────────────────────────────┘  │
│  ├──────┤  │  ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐            │
│  │persona│  │  │card│ │card│ │card│ │card│ │card│  waterfall │
│  │ nav   │  │  └────┘ └────┘ └────┘ └────┘ └────┘  (2–5 cols)│
│  ├──────┤  │  ...                                            │
│  │theme  │  │                                                │
│  └──────┘  │                                                │
└────────────┴───────────────────────────────────────────────┘
```

| Component | File | Responsibility |
|---|---|---|
| `App` | `src/App.tsx` | Owns state: persona list, active persona, recommendation, theme. Re-runs the pipeline when the persona changes; applies/persists the theme. |
| `Sidebar` | `src/components/Sidebar.tsx` | Brand, the persona switcher (nav; active in the accent), and the light/dark toggle. |
| `Feed` | `src/components/Feed.tsx` | `react-masonry-css` waterfall; responsive column counts; loads the cover manifest. |
| `NoteCard` | `src/components/NoteCard.tsx` | One card: cover → 2-line title → "why" caption → author + likes. |
| `TracePanel` | `src/components/TracePanel.tsx` | Collapsible DAG funnel: per-stage in→out, latency, sample ids, with a staggered reveal on update. |

## 3. Engine integration (`engine.ts`)

The engine ships as a single-file, classic script (`public/shuashua.js`, wasm
embedded) that attaches a global `ShuaShua()` factory. `engine.ts` loads it with
a `<script>` tag and exposes typed wrappers:

- `getPersonas()` → `Persona[]`
- `recommend(personaId)` → `Recommendation` (`{ persona, feed[], trace[] }`)

**Why a `<script>` tag, not an ESM `import`:** the engine lives in `public/`, and
Vite's dev server refuses to ESM-import `public/` assets (they may only be
referenced via HTML tags). A single-file classic build + script tag works
identically in dev, preview, and production. The module is loaded once and cached
(so the C++ store is built once per session).

## 4. Presentation layer

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

## 5. Theming

- Themes are CSS-variable sets: light in `:root`, dark under `[data-theme="dark"]`.
- A tiny inline script in `index.html` resolves the theme **before first paint**
  (order: `?theme=` query param → saved choice → OS preference) to avoid a flash.
- `App` reads the resolved theme, and the toggle persists the choice to
  `localStorage`. The `?theme=` param also makes the theme shareable/screenshotable.

## 6. The trace visualization

The DAG trace panel is the visible half of the project — "the trace is the
product." It renders the four stages as a left-to-right funnel showing
`in → out`, latency, and sample ids, with a staggered CSS reveal replayed on each
new recommendation. It is collapsible so the feed stays the focus.

Note on latency: the browser only reports sharp sub-millisecond timings when the
page is cross-origin-isolated (COOP/COEP), which the app requests. Headless
screenshots taken with a virtual time budget can show `0.0µs` — a capture
artifact, not a runtime bug; a real browser shows real microseconds.

## 7. Build

Standard Vite: `npm install`, `npm run dev` (dev server), `npm run build`
(type-check + production bundle to `dist/`), `npm run preview`. The engine
(`public/shuashua.js`) and covers (`public/covers/`) are static assets copied
into the build. See [Operations](Operations.md).
