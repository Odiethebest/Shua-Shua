// bench/bench_wasm.mjs — what the BROWSER actually pays.
//
// The deployed engine is web/public/shuashua.js (single-file, wasm embedded). Two
// things make its numbers differ from the native ones, and both matter:
//
//   1. emcc defines neither __ARM_NEON nor __wasm_simd128__, and build-wasm.sh
//      passes no -msimd128, so dot_simd compiles to its scalar fallback
//      (dot.hpp:86-90). The browser runs the SCALAR kernel.
//   2. The JS<->WASM boundary is text: two CSV strings in, one JSON string out.
//      That cost is invisible in the DAG trace, which only times operators.
//
// This measures per-operator latency (warmed, so JIT warm-up is not mistaken for
// C++ being slow) and the gap between the trace total and the JS wall clock.
//
// Run: node bench/bench_wasm.mjs

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname } from "node:path";
import { createRequire } from "node:module";

const enginePath = fileURLToPath(new URL("../web/public/shuashua.js", import.meta.url));
const code = readFileSync(enginePath, "utf8");
const factory = new Function("require", "__dirname", `${code}\n;return ShuaShua;`)(
  createRequire(import.meta.url),
  dirname(enginePath),
);
const engine = await factory();

const WEIGHTS = "1,0,0.5,0,0,0"; // food + travel, matching the native benches
const SEEN = "";
const NEW_RATIO = 100;
const WARMUP = 200;
const N = 500;

console.log("\n=== wasm — per operator and boundary overhead ===");
console.log(`  runtime        : Node ${process.versions.node} (V8 ${process.versions.v8})`);
console.log(`  engine         : web/public/shuashua.js (${code.length} chars)`);
console.log("  kernel backend : scalar fallback (emcc defines no __ARM_NEON, no -msimd128)");

// --- cold vs warm, stated explicitly so nobody quotes a JIT artefact ----------
const coldStart = process.hrtime.bigint();
const coldOut = engine.recommendFromProfile(WEIGHTS, SEEN, NEW_RATIO);
const coldEnd = process.hrtime.bigint();
const coldTrace = JSON.parse(coldOut).trace.reduce((s, t) => s + t.latency_us, 0);
console.log(
  `\n  COLD first call: trace ${coldTrace.toFixed(1)}us, wall ${(Number(coldEnd - coldStart) / 1000).toFixed(1)}us` +
    "  <- JIT warm-up, do not quote",
);

for (let i = 0; i < WARMUP; i++) engine.recommendFromProfile(WEIGHTS, SEEN, NEW_RATIO);

// --- warmed measurement ------------------------------------------------------
const sums = new Map();
const shape = new Map();
let wall = 0;
let outChars = 0;
for (let i = 0; i < N; i++) {
  const t0 = process.hrtime.bigint();
  const out = engine.recommendFromProfile(WEIGHTS, SEEN, NEW_RATIO);
  const t1 = process.hrtime.bigint();
  wall += Number(t1 - t0) / 1000;
  outChars = out.length;
  for (const t of JSON.parse(out).trace) {
    sums.set(t.name, (sums.get(t.name) ?? 0) + t.latency_us);
    shape.set(t.name, `${t.in} -> ${t.out}`);
  }
}

let traced = 0;
console.log(`\n  harness: warmed with ${WARMUP} calls, then mean of ${N} calls`);
console.log("\n  -- per operator (as the trace reports) --");
for (const [name, total] of sums) {
  const avg = total / N;
  traced += avg;
  console.log(`  ${`${name} ${shape.get(name)}`.padEnd(46)} ${avg.toFixed(2).padStart(9)} us`);
}
console.log(`  ${"SUM of trace latencies".padEnd(46)} ${traced.toFixed(2).padStart(9)} us`);

const wallAvg = wall / N;
console.log("\n  -- boundary --");
console.log(`  ${"payload in  (weightsCsv chars)".padEnd(46)} ${String(WEIGHTS.length).padStart(9)}`);
console.log(`  ${"payload out (json chars)".padEnd(46)} ${String(outChars).padStart(9)}`);
console.log(`  ${"JS wall clock per call".padEnd(46)} ${wallAvg.toFixed(2).padStart(9)} us`);
console.log(
  `  ${"boundary overhead (wall - trace)".padEnd(46)} ${(wallAvg - traced).toFixed(2).padStart(9)} us`,
);
console.log(
  `  ${"  ... as a share of wall clock".padEnd(46)} ${(((wallAvg - traced) / wallAvg) * 100).toFixed(1).padStart(8)}%`,
);
console.log(
  "\n  note: boundary = CSV parse (bindings.cpp:28-53) + to_json (api.hpp:186-221)\n" +
    "        + embind string marshalling both ways + JSON.parse (engine.ts:91).\n" +
    "        The payload is materialised 5 times per request.",
);
