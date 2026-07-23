# scripts/

Build and asset scripts (not part of the C++ engine). Full usage is in
[Operations](../Doc/en/Operations.md).

- `build-wasm.sh` — compile the engine to a single-file WASM module
  (`web/public/shuashua.js`) via Emscripten.
- `wasm_smoke.mjs` — load the WASM module in Node and validate the JSON boundary.
- `fetch-covers.mjs` — one-time, build-time fetch of Unsplash cover images into
  `web/public/covers/` (reads `UNSPLASH_KEY`; images are committed, no runtime key).
