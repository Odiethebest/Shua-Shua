// One-time, build-time fetch of cover images from Unsplash INTO the repo, so the
// deployed site makes zero Unsplash API calls at runtime and never ships a key.
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
const PER_CATEGORY = 40; // photos saved per category — raise for a bigger pool (more repo size + API pages)
const PAGE_SIZE = 20; // Unsplash caps a page at 30; we page to reach PER_CATEGORY
const UTM = "utm_source=shua_shua&utm_medium=referral";

// engine category (matches Note.category / the frontend) -> Unsplash search query
const CATEGORIES = [
  { key: "food", query: "food" },
  { key: "travel", query: "travel" },
  { key: "tech", query: "technology" },
  { key: "fashion", query: "fashion" },
];

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const coversDir = join(root, "web", "public", "covers");

function withUtm(url) {
  return url + (url.includes("?") ? "&" : "?") + UTM;
}

async function searchPage(query, page) {
  const url =
    "https://api.unsplash.com/search/photos" +
    `?query=${encodeURIComponent(query)}&orientation=portrait&per_page=${PAGE_SIZE}&page=${page}`;
  const res = await fetch(url, { headers: AUTH });
  if (!res.ok) throw new Error(`search "${query}" p${page} -> HTTP ${res.status} ${await res.text()}`);
  const data = await res.json();
  return Array.isArray(data.results) ? data.results : [];
}

// Collect up to PER_CATEGORY photos, paging as needed (a page caps at 30).
async function search(query) {
  const photos = [];
  const pages = Math.ceil(PER_CATEGORY / PAGE_SIZE);
  for (let page = 1; page <= pages && photos.length < PER_CATEGORY; page++) {
    const batch = await searchPage(query, page);
    if (batch.length === 0) break; // no more results for this query
    photos.push(...batch);
  }
  return photos.slice(0, PER_CATEGORY);
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

  // Phase 1: search + download images. Searches (4 categories x a few pages) stay
  // under the 50/hr limit; image downloads hit the CDN and don't count against it.
  for (const { key, query } of CATEGORIES) {
    try {
      const photos = await search(query);
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
