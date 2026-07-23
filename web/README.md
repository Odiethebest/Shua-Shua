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

## Notes

- The cross-origin isolation headers in `vite.config.ts` (COOP + COEP) unlock the
  browser's high-resolution timer, so the per-operator latencies in the trace are
  real microseconds. A static host must send the same two headers in production.
