# web/ — Shua Shua frontend (Milestone M4)

A Vite + React + TypeScript single-page app that renders the C++/WASM engine's
output as a Xiaohongshu-style feed, with a live DAG trace panel.

## Prerequisites

- Node 20+
- The WASM engine built into `public/`: run `../scripts/build-wasm.sh` (needs
  `emcc`). `public/shuashua.js` (single-file, wasm embedded) is a build artifact
  (gitignored).

## Develop / build

```bash
npm install
npm run dev       # dev server (http://localhost:5173)
npm run build     # type-check + production build -> dist/
npm run preview   # serve the production build
```

## How it fits together

- `src/engine.ts` loads `public/shuashua.js` (the compiled engine) via a
  `<script>` tag and calls `recommend(personaId)`, which returns the feed +
  per-operator trace as JSON.
- `src/presentation.ts` synthesizes cover / title / author / like-count
  (presentation only — the engine has none of these). The "why recommended" line
  is derived from the engine's real feature values.
- Components: `PersonaTabs` (the switcher), `Feed` (`react-masonry-css`),
  `NoteCard`, and `TracePanel` (the collapsible DAG funnel).

## Cover images (Unsplash, build-time)

Cover photos are fetched ONCE, locally, and committed to the repo — the deployed
site makes zero Unsplash API calls at runtime and ships no key.

```bash
UNSPLASH_KEY=your_access_key node scripts/fetch-covers.mjs
```

This downloads ~20 portrait photos per category into `web/public/covers/<category>/`
and writes `web/public/covers/manifest.json` (with attribution). Get a free Access
Key at <https://unsplash.com/developers>. The key is read from `UNSPLASH_KEY` at
fetch time only — never committed, never shipped to the browser.

At runtime the frontend (`src/covers.ts`) loads the local manifest and picks a
cover per note deterministically; the "Photo by … on Unsplash" attribution shows
on each cover as required. Cards fall back to gradient + emoji for any category
that has no images.

Note: Unsplash Demo tier is 50 requests/hour. The script does 4 searches + one
download-tracking ping per photo; past ~50 total calls the pings may be rejected,
but the images still download (the CDN isn't rate-limited).

## Notes

- The cross-origin isolation headers in `vite.config.ts` (COOP + COEP
  `require-corp`) unlock the browser's high-resolution timer, so the per-operator
  latencies in the trace are real microseconds. All resources are same-origin
  (engine wasm + local covers), so a static host just needs to send these two
  headers in production.
