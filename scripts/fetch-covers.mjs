// Build-time fetch of cover images from Unsplash INTO the repo (re-runnable — each
// run pulls a DIFFERENT random set), so the deployed site makes zero Unsplash API
// calls at runtime and never ships a key.
//
//   UNSPLASH_KEY=your_access_key node scripts/fetch-covers.mjs
//
// Output (committed to the repo):
//   web/public/covers/<category>/<n>.jpg
//   web/public/covers/manifest.json   (category -> [{ file, photographer, ... }])
//
// The key is read from the UNSPLASH_KEY env var at fetch time only; it is never
// hardcoded, never written to any file, and never shipped to the browser.
import { mkdir, writeFile, rm } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const KEY = process.env.UNSPLASH_KEY;
if (!KEY) {
  console.error("Missing UNSPLASH_KEY.\n  Run: UNSPLASH_KEY=xxx node scripts/fetch-covers.mjs");
  process.exit(1);
}

const AUTH = { Authorization: `Client-ID ${KEY}` };
// Photos saved per category. Override with the PER_CATEGORY env var to pull more:
//   PER_CATEGORY=150 UNSPLASH_KEY=xxx node scripts/fetch-covers.mjs
// Keep it <= ~180: Phase-1 random calls are (categories x (ceil(N/30)+2)) and must
// stay under Unsplash's 50 requests/hour (Demo tier) — 6 x 8 = 48 at N=180.
const PER_CATEGORY = Number(process.env.PER_CATEGORY) || 120;
const RANDOM_MAX = 30; // /photos/random caps count at 30 per request
const UTM = "utm_source=shua_shua&utm_medium=referral";

// engine category (matches Note.category / the frontend) -> Unsplash search query.
// ALL SIX engine categories must appear: v2's profile can weight any of them
// (e.g. Sports -> fitness, Literature -> beauty), and a missing category falls back
// to a bare gradient cover.
const CATEGORIES = [
  { key: "food", query: "food" },
  { key: "travel", query: "travel" },
  { key: "tech", query: "technology" },
  { key: "fashion", query: "fashion" },
  { key: "fitness", query: "fitness workout" },
  { key: "beauty", query: "skincare cosmetics" },
];

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const coversDir = join(root, "web", "public", "covers");

function withUtm(url) {
  return url + (url.includes("?") ? "&" : "?") + UTM;
}

// Fetch up to PER_CATEGORY photos for a query using /photos/random.
//
// WHY /photos/random and not /search/photos: search returns the same top results
// every time, so re-runs barely change the pool. The random endpoint returns a
// fresh set on each call, so every fetch produces DIFFERENT images. count caps at
// 30 per request, so we call a few times and dedupe by id (random batches can
// overlap between calls).
async function randomPhotos(query) {
  const byId = new Map();
  const maxCalls = Math.ceil(PER_CATEGORY / RANDOM_MAX) + 2; // +2 backfills dedupe losses at bigger pools
  for (let call = 0; call < maxCalls && byId.size < PER_CATEGORY; call++) {
    const count = Math.min(RANDOM_MAX, PER_CATEGORY - byId.size);
    const url =
      "https://api.unsplash.com/photos/random" +
      `?query=${encodeURIComponent(query)}&orientation=portrait&count=${count}`;
    const res = await fetch(url, { headers: AUTH });
    if (!res.ok) throw new Error(`random "${query}" -> HTTP ${res.status} ${await res.text()}`);
    const batch = await res.json();
    if (!Array.isArray(batch) || batch.length === 0) break;
    for (const photo of batch) byId.set(photo.id, photo);
  }
  return [...byId.values()].slice(0, PER_CATEGORY);
}

async function downloadImage(url, dest) {
  const res = await fetch(url); // images.unsplash.com CDN — not API rate-limited
  if (!res.ok) return false;
  await writeFile(dest, Buffer.from(await res.arrayBuffer()));
  return true;
}

async function main() {
  // Fresh start so photos removed from a category don't linger.
  await rm(coversDir, { recursive: true, force: true });
  await mkdir(coversDir, { recursive: true });

  const manifest = {};
  const downloadPings = []; // photo.links.download_location for photos we kept

  // Phase 1: fetch + download images. The random fetches (6 categories x a few
  // calls; ~30-40 requests at the default pool size) stay under the 50/hr limit;
  // image downloads hit the CDN for free. The download-tracking pings in Phase 2 do
  // exceed 50/hr — expected, and the images are saved regardless.
  for (const { key, query } of CATEGORIES) {
    try {
      const photos = await randomPhotos(query);
      const dir = join(coversDir, key);
      await mkdir(dir, { recursive: true });

      const entries = [];
      for (let i = 0; i < photos.length; i++) {
        const photo = photos[i];
        const ok = await downloadImage(photo.urls.small, join(dir, `${i}.jpg`));
        if (!ok) {
          console.warn(`  skip ${key}/${i}: image download failed`);
          continue;
        }
        entries.push({
          file: `covers/${key}/${i}.jpg`,
          photographer: photo.user.name,
          photographerUrl: withUtm(photo.user.links.html),
          unsplashUrl: `https://unsplash.com/?${UTM}`,
        });
        downloadPings.push(photo.links.download_location);
      }
      manifest[key] = entries;
      console.log(`${key}: ${entries.length} images`);
    } catch (e) {
      console.error(`ERROR ${key}: ${e.message}`);
      manifest[key] = manifest[key] ?? [];
    }
  }

  await writeFile(join(coversDir, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");
  const total = Object.values(manifest).reduce((n, a) => n + a.length, 0);
  console.log(`\nwrote web/public/covers/manifest.json (${total} images, ${Object.keys(manifest).length} categories)`);

  // Phase 2: required download tracking, one ping per kept photo. Best-effort —
  // these count against the 50/hr API limit, so past ~50 total calls they may be
  // rejected; the images are already saved regardless.
  console.log(`\npinging ${downloadPings.length} download endpoints (required, best-effort)...`);
  let ok = 0;
  let failed = 0;
  for (const loc of downloadPings) {
    try {
      const res = await fetch(loc, { headers: AUTH });
      res.ok ? ok++ : failed++;
    } catch {
      failed++;
    }
  }
  console.log(`download pings: ${ok} ok, ${failed} failed (rate limiting past ~50/hr is expected)`);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
