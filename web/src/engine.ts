// engine.ts — typed access to the WebAssembly recommendation engine.
//
// The engine is the C++ core compiled to WASM. It ships as a single-file, classic
// script (public/shuashua.js) that attaches a global `ShuaShua()` factory, with
// the .wasm embedded. We load it with a <script> tag: files in public/ can be
// referenced via HTML tags but NOT ESM-imported under Vite's dev server, and a
// tag also sidesteps bundling the emscripten glue. There is no ranking logic on
// the JS side — this is only the boundary.

export interface FeedItem {
  id: number;
  category: string;
  score: number;
  similarity: number;
  category_match: number;
  recency: number;
  popularity: number;
}

export interface TraceEntry {
  name: string;
  in: number;
  out: number;
  latency_us: number;
  sample_ids: number[];
}

export interface Recommendation {
  persona: string;
  feed: FeedItem[];
  trace: TraceEntry[];
}

export interface Persona {
  id: number;
  label: string;
}

// The functions embind exposes on the module instance.
interface EngineModule {
  recommend(personaId: number): string;
  recommendSimilar(itemId: number): string;
  recommendFromProfile(weightsCsv: string): string;
  personaCount(): number;
  personaLabel(index: number): string;
}

type EngineFactory = () => Promise<EngineModule>;

declare global {
  interface Window {
    ShuaShua?: EngineFactory;
  }
}

function loadScriptOnce(src: string): Promise<void> {
  return new Promise((resolve, reject) => {
    if (document.querySelector("script[data-shua-engine]") !== null) {
      resolve();
      return;
    }
    const script = document.createElement("script");
    script.src = src;
    script.async = true;
    script.setAttribute("data-shua-engine", "");
    script.addEventListener("load", () => resolve());
    script.addEventListener("error", () => reject(new Error(`failed to load ${src}`)));
    document.head.appendChild(script);
  });
}

// Load the engine once and instantiate it. Cached, so the C++ store is built
// exactly once per session (see the resident-store singleton in api.hpp).
let enginePromise: Promise<EngineModule> | null = null;

function loadEngine(): Promise<EngineModule> {
  if (enginePromise === null) {
    enginePromise = loadScriptOnce("/shuashua.js").then(() => {
      const factory = window.ShuaShua;
      if (factory === undefined) {
        throw new Error("engine factory (window.ShuaShua) not found");
      }
      return factory();
    });
  }
  return enginePromise;
}

export async function getPersonas(): Promise<Persona[]> {
  const engine = await loadEngine();
  const personas: Persona[] = [];
  for (let i = 0; i < engine.personaCount(); i++) {
    personas.push({ id: i, label: engine.personaLabel(i) });
  }
  return personas;
}

export async function recommend(personaId: number): Promise<Recommendation> {
  const engine = await loadEngine();
  return JSON.parse(engine.recommend(personaId)) as Recommendation;
}

// Item-based recall: recommend items similar to a clicked item (query = that
// item's own vector). Same JSON shape as recommend().
export async function recommendSimilar(itemId: number): Promise<Recommendation> {
  const engine = await loadEngine();
  return JSON.parse(engine.recommendSimilar(itemId)) as Recommendation;
}

// Profile-based recall (v2): the feed is built from the user's live profile. We pass
// the per-category weights as a CSV string (a fixed, tiny float vector — the
// simplest robust crossing of the embind boundary); C++ builds the query vector
// (make_query) and runs the same DAG. Same JSON shape as recommend().
export async function recommendFromProfile(categoryWeights: number[]): Promise<Recommendation> {
  const engine = await loadEngine();
  return JSON.parse(engine.recommendFromProfile(categoryWeights.join(","))) as Recommendation;
}
