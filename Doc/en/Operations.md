# Operations

Build, run, verify, and deploy Shua Shua. See [Architecture](Architecture.md) for
the system overview.

## 1. Toolchain

| Tool | Used for | Notes |
|---|---|---|
| Apple clang / any C++20 compiler | Native engine build + parity check | Present by default on macOS. |
| Emscripten (`emcc`) | WASM build | e.g. `brew install emscripten`. Not needed for the native build. |
| Node.js 20+ | Frontend, build scripts, smoke test | — |
| CMake | Optional (IDE only) | Not required; all builds are single commands. |

## 2. Build and run

### 2.1 Native engine (development + parity check)

```bash
clang++ -std=c++20 -O2 src/main.cpp -o shuashua && ./shuashua
```

Builds synthetic data, runs the pipeline, prints the feed, the DAG trace, the
`recommend()` JSON, and the naive-vs-SIMD recall parity + speedup. Expected to be
clean under `-Wall -Wextra`.

### 2.2 WebAssembly build

```bash
scripts/build-wasm.sh        # requires emcc on PATH
```

A single `emcc` invocation compiles `src/bindings.cpp` to a single-file module
`web/public/shuashua.js` (the `.wasm` is embedded). No CMake. Verify it in Node:

```bash
node scripts/wasm_smoke.mjs  # loads the module, prints personas + trace
```

### 2.3 Frontend

```bash
cd web
npm install
npm run dev        # dev server (http://localhost:5173)
npm run build      # type-check + production build → web/dist
npm run preview    # serve the production build
```

### 2.4 Cover images (build-time)

Cover photos are fetched once, locally, and committed to the repo, so the
deployed site makes zero Unsplash API calls at runtime and ships no key:

```bash
UNSPLASH_KEY=your_access_key node scripts/fetch-covers.mjs
```

Downloads ~40 **random** portrait photos per category (`PER_CATEGORY` in the
script) via `/photos/random`, so **each run pulls a different set**, into
`web/public/covers/<category>/` and writes `manifest.json` (with attribution). The
key is read from the `UNSPLASH_KEY` env var at fetch time only. Re-run any time to
refresh the pool; raise `PER_CATEGORY` to grow it.

> Unsplash Demo tier is 50 requests/hour. The script does a couple of random
> fetches per category (count caps at 30) + one download-tracking ping per photo;
> past ~50 total calls the pings may be rejected, but the images still download
> (the CDN is not rate-limited).

## 3. Configuration and secrets

- **No runtime API key.** The only key use is the local `fetch-covers.mjs` script
  (an env var, never written to a file, never committed, never shipped to the
  browser). `.env*` files are gitignored; the committed manifest and images
  contain no key.
- **Committed build artifacts:** `web/public/covers/` (images + manifest) is
  committed. The generated engine `web/public/shuashua.js`, `web/dist/`, and
  `node_modules/` are gitignored.

## 4. Deployment

The deliverable is the static output of `npm run build` (`web/dist/`), served by
any static host. Two requirements:

1. **Cross-origin isolation headers** on the document (and ideally all responses):
   ```
   Cross-Origin-Opener-Policy: same-origin
   Cross-Origin-Embedder-Policy: require-corp
   ```
   These unlock the browser's high-resolution timer so the trace latencies are
   real microseconds. All resources are same-origin, so nothing else is needed.
   (Netlify `_headers`, Vercel `headers`, or an `nginx add_header` all work.)
2. **Correct MIME + serving** for `.wasm`/`.js`/`.json` static assets (default on
   most hosts). The engine and covers load from the site root.

## 5. Verification checklist

- Native build clean under `-Wall -Wextra`; `./shuashua` prints the feed + trace.
- Recall parity: `result diff = 0`, max score delta ~1e-7, SIMD scan faster than
  scalar.
- `node scripts/wasm_smoke.mjs` prints personas, valid JSON, and the funnel.
- `npm run build` type-checks and bundles cleanly.
- The built bundle contains no `api.unsplash.com` — zero runtime Unsplash calls.

## 6. Roadmap & status

| Milestone | Contents | Status |
|---|---|---|
| M0 — Kernel | `Note`, SoA store, synthetic data, naive recall, stdout | done |
| M1 — DAG | Operator interface, scheduler, four operators, trace | done |
| M2 — SIMD + diff | NEON recall kernel, naive/SIMD parity (diff = 0), speedup | done |
| M3 — WASM | Emscripten build, `recommend` bound to JS, JSON boundary | done |
| M4 — Feed UI | React feed, persona switcher, "why", DAG trace panel; Xiaohongshu-web restyle; dark mode; build-time cover images | done |
| M5 — Ship | Deploy the static build (with the COOP/COEP headers above) | pending |
| Stretch | HNSW index, int8 quantization, learned embeddings, WASM SIMD recall | later |

### Engineering conventions

- **Readability over cleverness**; the naive recall path is kept permanently as
  the parity reference.
- **The SIMD kernel** ships as a clearly-commented scalar reference plus a
  minimal NEON path that must produce identical ranking output.
- **Comments explain the "why"** (the design decision and the rejected
  alternative), not the "what".
