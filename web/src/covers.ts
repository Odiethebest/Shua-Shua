// covers.ts — local cover images fetched at build time (scripts/fetch-covers.mjs)
// and committed under web/public/covers/. The site makes NO Unsplash API calls at
// runtime and ships no key; it only reads the local manifest + images.

export interface Cover {
  file: string; // e.g. "covers/food/0.jpg", relative to the site root
  photographer: string;
  photographerUrl: string;
  unsplashUrl: string;
}
type Manifest = Record<string, Cover[]>;

let manifest: Manifest | null = null;
let loadPromise: Promise<void> | null = null;

// Load the manifest once. Never rejects — a missing/failed manifest simply means
// every card falls back to the gradient placeholder.
export function loadCovers(): Promise<void> {
  if (manifest !== null) return Promise.resolve();
  if (loadPromise !== null) return loadPromise;
  loadPromise = (async () => {
    try {
      const res = await fetch("/covers/manifest.json");
      manifest = res.ok ? ((await res.json()) as Manifest) : {};
    } catch {
      manifest = {};
    }
  })();
  return loadPromise;
}

export function coversLoaded(): boolean {
  return manifest !== null;
}

// Deterministic cover for a note: same id -> same cover, no reshuffle on refresh.
export function coverFor(category: string, id: number): Cover | null {
  if (manifest === null) return null;
  const list = manifest[category];
  if (list === undefined || list.length === 0) return null;
  let x = (id * 2654435761 + 0x9e3779b9) >>> 0;
  x = (x ^ (x >>> 15)) >>> 0;
  return list[x % list.length];
}
