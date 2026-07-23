import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Cross-origin isolation (COOP + COEP). This unlocks the browser's
// high-resolution timer, so the per-operator latencies in the DAG trace are real
// microseconds instead of being coarsened to zero. The engine assets are
// same-origin, so isolation does not affect loading. A static host must send
// these same two headers in production for the timings to stay sharp.
//
// All resources are same-origin (engine wasm + local cover images), so plain
// `require-corp` is enough — no cross-origin subresources to accommodate.
const crossOriginIsolation = {
  "Cross-Origin-Opener-Policy": "same-origin",
  "Cross-Origin-Embedder-Policy": "require-corp",
};

export default defineConfig({
  plugins: [react()],
  server: { headers: crossOriginIsolation },
  preview: { headers: crossOriginIsolation },
});
