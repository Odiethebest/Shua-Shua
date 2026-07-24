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

// A short human summary for the sidebar (B3 expands this into a live panel).
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
