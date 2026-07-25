// profile.ts — the v2 user profile: a behavior-driven interest model persisted in
// the browser. v2 replaces v1's fixed personas with this living profile.
//
// This block (B1) introduces the MODEL + PERSISTENCE only. Later blocks seed it
// from a cold-start tag picker (B2), grow it from clicks (B3), decay it (B4), and
// turn it into the recall query vector (B5). No engine change here.

// The interest tags. The tag set is exactly the engine's six item categories
// (one-to-one), so ONE taxonomy is used everywhere — the cold-start picker, the
// profile panel, the "driven by" line, and each card's "Because you're into …" label
// all speak the same words. (An earlier version used a larger set that folded
// many-to-one onto the six categories, which meant a card could show a category — e.g.
// "beauty" — for a tag the user never saw; collapsing to the categories removed that.)
export const TAGS = [
  "Food", "Fashion", "Travel", "Tech", "Fitness", "Beauty",
] as const;
export type Tag = (typeof TAGS)[number];

// Each tag maps to its engine category (the lowercase names in CATEGORY_ORDER /
// api.hpp). The mapping is 1:1; kept as an explicit table (not just toLowerCase) so
// the tag→category contract stays in one obvious place.
export const TAG_TO_CATEGORY: Record<Tag, string> = {
  Food: "food",
  Fashion: "fashion",
  Travel: "travel",
  Tech: "tech",
  Fitness: "fitness",
  Beauty: "beauty",
};

// The engine's six item categories, in the SAME order as api.hpp's CATEGORY_NAMES
// (food, fashion, travel, tech, fitness, beauty). categoryWeights() must emit a
// vector in this order because the C++ side indexes centroids by it — this is the
// one place the two languages must agree on ordering.
export const CATEGORY_ORDER = [
  "food", "fashion", "travel", "tech", "fitness", "beauty",
] as const;

// The new/seen mix ratio (v2 · B6): target % of a refreshed page that should be NEW
// (unseen). The rest is filled with top-scoring already-seen items, so ~80 leaves a
// small 2–3 card "favorites" quota in a 12-card page — exploration with a little
// exploitation. Passed to the engine's MixOp.
export const NEW_RATIO = 80;

export interface ClickRecord {
  itemId: number;
  tags: string[]; // tags attributed to the clicked item
  timestamp: number; // Date.now() at click
}

export interface Profile {
  tagWeights: Record<string, number>; // accumulated (later: decayed) weight per tag
  clickHistory: ClickRecord[]; // for decay + the "seen" set
  seenItemIds: Set<number>; // supports the new/seen mix on refresh (B6)
  onboarded: boolean; // has the user passed cold-start onboarding (B2)
}

// A neutral profile: equal weight to every tag. WHY: at cold start with no chosen
// tags (or a corrupt/empty store), a uniform profile yields a diverse sampler feed
// rather than an empty one; the first clicks then specialize it (cold-start →
// warm-up).
export function neutralProfile(): Profile {
  const tagWeights: Record<string, number> = {};
  for (const tag of TAGS) tagWeights[tag] = 1;
  return { tagWeights, clickHistory: [], seenItemIds: new Set(), onboarded: false };
}

// Build the initial profile from the cold-start tag picker (B2). Selected tags get
// weight 1, the rest 0. An empty selection (the user skipped) falls back to the
// neutral profile — a diverse sampler — so the first feed is never empty.
// Either way the profile is marked onboarded so the picker won't show again.
export function seededProfile(selectedTags: string[]): Profile {
  if (selectedTags.length === 0) {
    return { ...neutralProfile(), onboarded: true };
  }
  const tagWeights: Record<string, number> = {};
  for (const tag of TAGS) tagWeights[tag] = selectedTags.includes(tag) ? 1 : 0;
  return { tagWeights, clickHistory: [], seenItemIds: new Set(), onboarded: true };
}

const STORAGE_KEY = "shua-profile-v3"; // v3: tags = the 6 engine categories (was a larger, lossy set)

// Persisted shape: a Set is not JSON-serializable, so seenItemIds is stored as an
// array and rehydrated on load.
interface PersistedProfile {
  tagWeights: Record<string, number>;
  clickHistory: ClickRecord[];
  seenItemIds: number[];
  onboarded: boolean;
}

// Where the profile lives (v2 · B8 session control):
//   "local"   → localStorage: survives launches ("Remember me on this device" ON).
//   "session" → sessionStorage: dropped when the tab closes, so each launch starts
//               fresh at cold start ("Remember me" OFF — the DEFAULT, best for the
//               dev/demo case where a clean start each time is wanted).
// This is NOT auth: it only chooses how long local state lives, not who anyone is.
export type StorageMode = "local" | "session";
export const DEFAULT_STORAGE_MODE: StorageMode = "session";

// The Storage object for a mode, or null if it can't be reached (private mode can make
// even touching window.localStorage throw). Callers treat null as "no storage."
function storageFor(mode: StorageMode): Storage | null {
  try {
    return mode === "local" ? window.localStorage : window.sessionStorage;
  } catch {
    return null;
  }
}

// Parse the profile stored under one mode, or null if absent / blocked / corrupt.
function readProfile(mode: StorageMode): Profile | null {
  const storage = storageFor(mode);
  if (storage === null) return null;
  try {
    const raw = storage.getItem(STORAGE_KEY);
    if (raw === null) return null;
    const parsed = JSON.parse(raw) as Partial<PersistedProfile>;
    if (parsed === null || typeof parsed !== "object" || parsed.tagWeights === undefined) {
      return null;
    }
    return {
      tagWeights: parsed.tagWeights,
      clickHistory: Array.isArray(parsed.clickHistory) ? parsed.clickHistory : [],
      seenItemIds: new Set(Array.isArray(parsed.seenItemIds) ? parsed.seenItemIds : []),
      onboarded: parsed.onboarded === true,
    };
  } catch {
    return null;
  }
}

// Load the profile, preferring a *remembered* (localStorage) one over a session one,
// and report which storage it came from so saves go back to the same place. On a fresh
// launch — nothing stored, storage unusable, or only a not-onboarded remnant — return a
// neutral profile in the DEFAULT mode, which makes the app show the cold-start picker
// (B2). WHY prefer local: if the user ever chose "remember me," that persistent
// profile is authoritative and a stale session copy must not shadow it.
export function loadProfile(): { profile: Profile; mode: StorageMode } {
  const local = readProfile("local");
  if (local !== null && local.onboarded) return { profile: local, mode: "local" };
  const session = readProfile("session");
  if (session !== null && session.onboarded) return { profile: session, mode: "session" };
  return { profile: neutralProfile(), mode: DEFAULT_STORAGE_MODE };
}

// Persist the profile to the storage chosen by `mode`, and remove it from the OTHER
// storage so exactly one copy exists (flipping "remember me" ON moves it from session
// to local, and vice versa). A not-onboarded profile is never persisted (fresh/neutral,
// or just reset) — it is cleared instead, so the next load starts at cold start.
// Best-effort: if storage is full or blocked, keep the in-memory profile for this
// session rather than throwing.
export function saveProfile(profile: Profile, mode: StorageMode): void {
  const other: StorageMode = mode === "local" ? "session" : "local";
  try {
    storageFor(other)?.removeItem(STORAGE_KEY);
  } catch {
    /* ignore */
  }
  try {
    const active = storageFor(mode);
    if (active === null) return;
    if (!profile.onboarded) {
      active.removeItem(STORAGE_KEY);
      return;
    }
    const persisted: PersistedProfile = {
      tagWeights: profile.tagWeights,
      clickHistory: profile.clickHistory,
      seenItemIds: [...profile.seenItemIds],
      onboarded: profile.onboarded,
    };
    active.setItem(STORAGE_KEY, JSON.stringify(persisted));
  } catch {
    /* storage unavailable — ignore */
  }
}

// Clear the persisted profile + click history from BOTH storages (v2 · B8 — "start
// over"). With it gone the next load finds nothing and falls back to a fresh neutral
// profile — the user becomes brand-new and cold start runs again. NOT a logout: there
// is no account, session token, username, or backend — only local state being wiped.
export function clearProfile(): void {
  for (const mode of ["local", "session"] as const) {
    try {
      storageFor(mode)?.removeItem(STORAGE_KEY);
    } catch {
      /* ignore */
    }
  }
}

// Record a click as implicit feedback (v2 · B3): bump the weight of the tag for the
// clicked item's category, append to click history, and mark the item seen. Returns a
// NEW profile (immutable) so React re-renders the live panel. With the 1:1 taxonomy a
// click bumps exactly one tag (the item's own category).
export function recordClick(profile: Profile, itemId: number, category: string): Profile {
  const tags = TAGS.filter((tag) => TAG_TO_CATEGORY[tag] === category);
  const tagWeights = { ...profile.tagWeights };
  for (const tag of tags) tagWeights[tag] = (tagWeights[tag] ?? 0) + 1;
  return {
    ...profile,
    tagWeights,
    clickHistory: [...profile.clickHistory, { itemId, tags, timestamp: Date.now() }],
    seenItemIds: new Set(profile.seenItemIds).add(itemId),
  };
}

// Interest decay (v2 · B4). We use PER-REFRESH multiplicative decay: each refresh
// multiplies every tag weight by DECAY_FACTOR (< 1). We rejected time-based
// exp(-λΔt) decay because in a click-driven demo almost no wall-clock time passes,
// so it would look like nothing ever fades; event-based decay makes the fade happen
// exactly when the user acts. New clicks enter at full weight (see recordClick), so
// tags you keep feeding stay high while tags you stop feeding shrink each refresh —
// recent interest outweighs stale interest.
//
// DECAY_FACTOR is the PLASTICITY knob: the fraction of an un-fed tag's weight that
// survives each refresh. Lower = shorter memory = recent behavior shifts the profile
// faster; higher = more stable but risks ENTRENCHMENT (a tag clicked heavily early
// stays dominant for many refreshes). Clicks accumulate unbounded (+1 each, no cap),
// so only decay pulls a big early tag back down — at 0.7 that took too many refreshes
// and the profile felt locked to early clicks. 0.5 (halve per refresh) keeps a clear
// preference while letting a sustained change of interest take over within a few
// refreshes.
export const DECAY_FACTOR = 0.5;

export function decayProfile(profile: Profile, factor = DECAY_FACTOR): Profile {
  const tagWeights: Record<string, number> = {};
  for (const [tag, w] of Object.entries(profile.tagWeights)) tagWeights[tag] = w * factor;
  // Guard: if everything has decayed to ~0, reset to neutral so the profile (and
  // the recall query built from it in B5) never becomes a zero/NaN vector.
  if (Math.max(0, ...Object.values(tagWeights)) < 1e-3) {
    return { ...profile, tagWeights: neutralProfile().tagWeights };
  }
  return { ...profile, tagWeights };
}

// Map the six tag weights to six per-category weights (via TAG_TO_CATEGORY), in
// CATEGORY_ORDER. With the 1:1 taxonomy this is a straight reordering into the order
// the engine indexes centroids by. The vector-space math (the weighted centroid blend)
// happens in C++ (api.hpp make_query) so it can't drift from the persona path. The
// result is what the engine turns into the recall query vector (v2 · B5).
export function categoryWeights(profile: Profile): number[] {
  const w = new Array<number>(CATEGORY_ORDER.length).fill(0);
  for (const tag of TAGS) {
    const idx = (CATEGORY_ORDER as readonly string[]).indexOf(TAG_TO_CATEGORY[tag]);
    if (idx >= 0) w[idx] += profile.tagWeights[tag] ?? 0;
  }
  return w;
}

// A short human summary of the profile (the sidebar now renders it as a live panel).
export function summarizeProfile(profile: Profile): string {
  const entries = Object.entries(profile.tagWeights);
  const max = Math.max(0, ...entries.map(([, w]) => w));
  if (max <= 0) return "empty";
  // A uniform profile (every tag equal) is the neutral / skipped state.
  if (entries.every(([, w]) => w === max)) return "neutral (all interests)";
  const top = entries
    .filter(([, w]) => w > 0)
    .sort((a, b) => b[1] - a[1])
    .slice(0, 3)
    .map(([tag]) => tag);
  const clicks = profile.clickHistory.length;
  return clicks > 0 ? `${top.join(", ")} · ${clicks} clicks` : top.join(", ");
}
