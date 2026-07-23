// presentation.ts — deterministic, frontend-only "content" for each note.
//
// The engine deals in ids, categories and numeric features; it has NO titles,
// authors, cover images or like counts (by design — those are presentation, per
// the README). We synthesize them here so the feed looks like a real lifestyle
// app while staying fully offline. The one thing tied to the real engine is the
// "why recommended" line, derived from the item's actual feature values.

import type { FeedItem } from "./engine";

// --- deterministic hashing (same id -> same content, every render) ---
function hash(id: number, salt: number): number {
  let x = (id * 2654435761 + salt * 40503 + 0x9e3779b9) >>> 0;
  x = Math.imul(x ^ (x >>> 16), 0x21f0aaad) >>> 0;
  x = Math.imul(x ^ (x >>> 15), 0x735a2d97) >>> 0;
  return (x ^ (x >>> 15)) >>> 0;
}
function pick<T>(arr: readonly T[], id: number, salt: number): T {
  return arr[hash(id, salt) % arr.length];
}

const TITLES: Record<string, readonly string[]> = {
  food: [
    "A tiny ramen bar worth the wait", "15-minute garlic butter noodles",
    "Best brunch spots in town", "Homemade dumplings, fold by fold",
    "The creamiest matcha latte at home", "Street food crawl: what to order",
    "One-pan weeknight dinners", "The only pancake recipe I use",
    "Cozy soups for cold nights", "A perfect cup of pour-over",
  ],
  travel: [
    "48 hours in Lisbon on a budget", "Hidden beaches most people miss",
    "A slow morning in Kyoto", "Night train diaries: the Alps",
    "Cafe-hopping through Melbourne", "Where to catch the sunset here",
    "Packing light for two weeks", "A weekend in the fjords",
    "Old town walks you shouldn't skip", "Island-hopping, mapped out",
  ],
  tech: [
    "My minimal desk setup for 2026", "Why I switched to a split keyboard",
    "Tiny apps that saved my week", "Building a home server, part 1",
    "The mechanical keyboard rabbit hole", "E-ink tablet: two weeks in",
    "My terminal, finally tidy", "Self-hosting on a $5 box",
    "The cables drawer, solved", "A calmer notifications setup",
  ],
  fashion: [
    "Capsule wardrobe: 10 pieces", "Thrifted fits under $30",
    "How I style one white shirt", "Autumn layers that actually work",
    "The perfect everyday tote", "Linen season starter pack",
    "Neutral tones, styled 5 ways", "Denim that lasts years",
    "One dress, day to night", "Rainy-day outfit formula",
  ],
  fitness: [
    "Beginner pull-up progression", "My 20-minute morning mobility",
    "Running my first 10k", "3 moves for a stronger back",
    "Home gym on a small budget", "Rest days matter — here's why",
    "Kettlebell basics that stick", "Stretching I actually keep up",
    "Zone 2, explained simply", "A 5-minute desk reset",
  ],
  beauty: [
    "5-minute everyday makeup", "The glass-skin routine, simplified",
    "Drugstore dupes that actually work", "The SPF I reapply every day",
    "Lip combos for cool tones", "How I fixed my dry winter skin",
    "A fragrance layering guide", "Gentle cleansers that don't strip",
    "My 3-step night routine", "Blush placement, finally clicked",
  ],
};
const FALLBACK_TITLES: readonly string[] = [
  "A little something I found", "Saving this one for later", "Had to share this",
];

const AUTHORS: readonly string[] = [
  "mochi.days", "june.hikes", "pixeldiane", "chen.eats", "atlas.walks", "nori",
  "kaya.studio", "sol.and.moon", "tin.makes", "hana.rey", "the.slow.cook",
  "mika", "leo.builds", "wren", "juno.fit", "coco.skin",
];

// Per-category cover treatment: an emoji plus a base hue. The actual gradient is
// derived per-id from the hue (with a little jitter) so same-category covers are
// clearly a family but no two look identical. Fully offline — no image requests.
const CATEGORY_STYLE: Record<string, { emoji: string; hue: number }> = {
  food: { emoji: "🍜", hue: 18 },
  travel: { emoji: "✈️", hue: 210 },
  tech: { emoji: "💻", hue: 255 },
  fashion: { emoji: "👗", hue: 330 },
  fitness: { emoji: "🏋️", hue: 150 },
  beauty: { emoji: "💄", hue: 300 },
};
const FALLBACK_STYLE = { emoji: "📌", hue: 220 };

// Cover aspect ratios (height / width). A spread of values gives the masonry its
// varied-height look.
const ASPECTS: readonly number[] = [0.92, 1.1, 1.25, 1.4, 1.5];

export interface CardContent {
  title: string;
  author: string;
  avatar: { letter: string; gradient: string };
  likes: string;
  cover: { gradient: string; emoji: string; aspect: number };
  why: string;
}

function formatCount(n: number): string {
  if (n >= 10000) return `${Math.round(n / 1000)}k`;
  if (n >= 1000) return `${(n / 1000).toFixed(1)}k`;
  return String(n);
}

function categoryOffset(category: string): number {
  let sum = 0;
  for (let i = 0; i < category.length; i++) sum += category.charCodeAt(i);
  return sum;
}

// The "why recommended" line, tied to the real ranking. Similarity is the recall
// reason shared by every recalled item, so we surface the strongest *ranking*
// signal among the differentiating features — weighted the same way ScoreOp
// weights them (category 0.5, recency 0.3, popularity 0.2) so the caption matches
// what actually moved this item up.
function whyFor(item: FeedItem): string {
  const signals = [
    { key: "category", value: item.category_match * 0.5 },
    { key: "recency", value: item.recency * 0.3 },
    { key: "popularity", value: item.popularity * 0.2 },
  ];
  signals.sort((a, b) => b.value - a.value);
  switch (signals[0].key) {
    case "category":
      return `Because you're into ${item.category}`;
    case "recency":
      return "Fresh — just posted";
    case "popularity":
      return "Trending right now";
    default:
      return "Picked for you";
  }
}

// `variant` is the item's index among same-category items in the current feed.
// Using it to pick the title guarantees no two same-category cards repeat a title
// (as long as the pool is at least as large as the per-category count).
export function contentFor(item: FeedItem, variant: number): CardContent {
  const style = CATEGORY_STYLE[item.category] ?? FALLBACK_STYLE;
  const titles = TITLES[item.category] ?? FALLBACK_TITLES;

  const author = pick(AUTHORS, item.id, 2);
  const avatarHue = hash(item.id, 6) % 360;
  const hue = style.hue + ((hash(item.id, 7) % 26) - 13);
  const angle = 120 + (hash(item.id, 8) % 80);
  const likeCount = Math.round(item.popularity * 4200) + (hash(item.id, 3) % 900);

  return {
    title: titles[(categoryOffset(item.category) + variant) % titles.length],
    author,
    avatar: {
      letter: author.charAt(0).toUpperCase(),
      gradient: `linear-gradient(135deg, hsl(${avatarHue} 68% 64%), hsl(${(avatarHue + 40) % 360} 68% 52%))`,
    },
    likes: formatCount(likeCount),
    cover: {
      gradient: `linear-gradient(${angle}deg, hsl(${hue} 88% 63%), hsl(${hue + 16} 82% 50%))`,
      emoji: style.emoji,
      aspect: pick(ASPECTS, item.id, 5),
    },
    why: whyFor(item),
  };
}
