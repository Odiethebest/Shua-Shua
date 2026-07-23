// Smoke test for the WASM build (Milestone M3): load the module in Node, call the
// bound entry points, and validate the JSON boundary output — the same contract
// the M4 frontend will consume. No browser needed.
//
//   Build first: scripts/build-wasm.sh
//   Run:         node scripts/wasm_smoke.mjs
import createModule from "../web/shuashua.mjs";

const m = await createModule();

const count = m.personaCount();
console.log(`personaCount = ${count}`);
for (let i = 0; i < count; i++) {
  console.log(`  persona[${i}] = ${m.personaLabel(i)}`);
}

const json = m.recommend(2); // "Foodie + Traveler"
const result = JSON.parse(json);
console.log("valid JSON ✓");
console.log("persona:", result.persona);
console.log("feed items:", result.feed.length);
console.log(
  "trace:",
  result.trace.map((t) => `${t.name}(${t.in}->${t.out})`).join(" -> "),
);
