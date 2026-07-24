// profile.ts — the v2 user profile: a behavior-driven interest model persisted in
// the browser. v2 replaces v1's fixed personas with this living profile.
//
// This block (B1) introduces the MODEL + PERSISTENCE only. Later blocks seed it
// from a cold-start tag picker (B2), grow it from clicks (B3), decay it (B4), and
// turn it into the recall query vector (B5). No engine change here.

// The cold-start tag set (v2design §3).
export const TAGS = [
  "Travel", "Food", "Tech", "News", "Art", "Sports", "Literature", "Outdoors",
] as const;
export type Tag = (typeof TAGS)[number];

// Tags map onto the engine's six item categories (kept in ONE place, per §4.1), so
// a tag weight translates directly into item-vector space. The tag set is larger
// than the category set, so several tags fold onto one category — a deliberate,
// documented approximation (the synthetic data has no separate "news" or
// "literature" item vectors).
export const TAG_TO_CATEGORY: Record<Tag, string> = {
  Travel: "travel",
  Food: "food",
  Tech: "tech",
  News: "tech",
  Art: "fashion",
  Sports: "fitness",
  Literature: "beauty",
  Outdoors: "travel",
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
// rather than an empty one; the first clicks then specialize it (§6, cold-start →
// warm-up).
export function neutralProfile(): Profile {
  const tagWeights: Record<string, number> = {};
  for (const tag of TAGS) tagWeights[tag] = 1;
  return { tagWeights, clickHistory: [], seenItemIds: new Set(), onboarded: false };
}

// Build the initial profile from the cold-start tag picker (B2). Selected tags get
// weight 1, the rest 0. An empty selection (the user skipped) falls back to the
// neutral profile — a diverse sampler — so the first feed is never empty (§6).
// Either way the profile is marked onboarded so the picker won't show again.
export function seededProfile(selectedTags: string[]): Profile {
  if (selectedTags.length === 0) {
    return { ...neutralProfile(), onboarded: true };
  }
  const tagWeights: Record<string, number> = {};
  for (const tag of TAGS) tagWeights[tag] = selectedTags.includes(tag) ? 1 : 0;
  return { tagWeights, clickHistory: [], seenItemIds: new Set(), onboarded: true };
}

const STORAGE_KEY = "shua-profile-v2";

// Persisted shape: a Set is not JSON-serializable, so seenItemIds is stored as an
// array and rehydrated on load.
interface PersistedProfile {
  tagWeights: Record<string, number>;
  clickHistory: ClickRecord[];
  seenItemIds: number[];
  onboarded: boolean;
}

// Load the profile from local storage. WHY try/catch + fallback: storage may be
// absent, disabled, or corrupt (and reading a blocked key can throw); on any
// failure we start fresh with a neutral profile rather than crash (§6).
export function loadProfile(): Profile {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw === null) return neutralProfile();
    const parsed = JSON.parse(raw) as Partial<PersistedProfile>;
    if (parsed === null || typeof parsed !== "object" || parsed.tagWeights === undefined) {
      return neutralProfile();
    }
    return {
      tagWeights: parsed.tagWeights,
      clickHistory: Array.isArray(parsed.clickHistory) ? parsed.clickHistory : [],
      seenItemIds: new Set(Array.isArray(parsed.seenItemIds) ? parsed.seenItemIds : []),
      onboarded: parsed.onboarded === true,
    };
  } catch {
    return neutralProfile();
  }
}

// Save the profile to local storage. Best-effort: if storage is full or blocked,
// keep the in-memory profile for this session rather than throwing.
export function saveProfile(profile: Profile): void {
  try {
    const persisted: PersistedProfile = {
      tagWeights: profile.tagWeights,
      clickHistory: profile.clickHistory,
      seenItemIds: [...profile.seenItemIds],
      onboarded: profile.onboarded,
    };
    localStorage.setItem(STORAGE_KEY, JSON.stringify(persisted));
  } catch {
    /* storage unavailable — ignore */
  }
}

// Record a click as implicit feedback (v2 · B3): bump the weight of every tag that
// maps to the clicked item's category, append to click history, and mark the item
// seen. Returns a NEW profile (immutable) so React re-renders the live panel.
// Note: several tags fold onto one category, so clicking a tech item bumps every
// tag mapped to "tech" (Tech and News) — a consequence of the coarse category space.
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
  // §6 guard: if everything has decayed to ~0, reset to neutral so the profile (and
  // the recall query built from it in B5) never becomes a zero/NaN vector.
  if (Math.max(0, ...Object.values(tagWeights)) < 1e-3) {
    return { ...profile, tagWeights: neutralProfile().tagWeights };
  }
  return { ...profile, tagWeights };
}

// Collapse the 8 tag weights into 6 per-category weights (via TAG_TO_CATEGORY), in
// CATEGORY_ORDER. This is the ONLY tag→category translation; the vector-space math
// (the weighted centroid blend) happens in C++ (api.hpp make_query) so it can't
// drift from the persona path. The result is what the engine turns into the recall
// query vector (v2 · B5).
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
