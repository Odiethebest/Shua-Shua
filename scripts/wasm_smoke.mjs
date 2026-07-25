// Smoke test for the WASM build (Milestone M3): load the single-file engine in
// Node and validate the JSON boundary — the same contract the M4 frontend
// consumes. No browser needed.
//
//   Build first: scripts/build-wasm.sh
//   Run:         node scripts/wasm_smoke.mjs
//
// The build is a classic MODULARIZE + SINGLE_FILE script that defines a global
// `ShuaShua` factory (rather than an ES module), so we read the file and evaluate
// it — handing it `require`/`__dirname` for its Node branch — then grab the
// factory it declares.
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname } from "node:path";
import { createRequire } from "node:module";

const scriptPath = fileURLToPath(new URL("../web/public/shuashua.js", import.meta.url));
const code = readFileSync(scriptPath, "utf8");
const factory = new Function("require", "__dirname", `${code}\n;return ShuaShua;`)(
  createRequire(import.meta.url),
  dirname(scriptPath),
);

const engine = await factory();

// Exercise the sole entry point: a "For you" profile query (food + travel blend).
// Weights are the six per-category weights (CATEGORY_ORDER: food, fashion, travel,
// tech, fitness, beauty) as a CSV; empty seen set; new_ratio 100.
const result = JSON.parse(engine.recommendFromProfile("0.5,0,0.5,0,0,0", "", 100));
console.log("valid JSON ✓");
console.log("label:", result.persona);
console.log("feed items:", result.feed.length);
console.log(
  "trace:",
  result.trace.map((t) => `${t.name}(${t.in}->${t.out}, ${t.latency_us}us)`).join(" -> "),
);
