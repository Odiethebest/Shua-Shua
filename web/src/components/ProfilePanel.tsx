import { TAGS, type Profile } from "../profile";

// The live profile panel (v2) — the core of this version, so it's a prominent card:
// a big click count, the top interests called out, and a bar per tag that grows and
// reorders in real time as you click. It reads as "your behavior built this."
export default function ProfilePanel({ profile }: { profile: Profile }) {
  const weights = TAGS.map((tag) => ({ tag, w: profile.tagWeights[tag] ?? 0 }));
  const max = Math.max(1, ...weights.map((x) => x.w));
  const sorted = [...weights].sort((a, b) => b.w - a.w);
  const clicks = profile.clickHistory.length;

  // Neutral = every tag equal (fresh/skipped, or fully decayed): no preference to
  // highlight yet. Otherwise the leading tags are the top interests.
  const isNeutral = weights.every((x) => x.w === weights[0].w);
  const top = sorted
    .filter((x) => x.w > 0)
    .slice(0, 2)
    .map((x) => x.tag);

  const fmt = (w: number) => (Number.isInteger(w) ? String(w) : w.toFixed(1));
  const fillPct = (w: number) => (isNeutral ? 38 : (w / max) * 100);

  return (
    <div className="profile-panel">
      <div className="profile-panel-head">
        <span className="profile-panel-title">Your profile</span>
        <span className="profile-panel-tag">behavior-built</span>
      </div>

      <div className="profile-hero">
        <span className="profile-hero-num">{clicks}</span>
        <span className="profile-hero-label">{clicks === 1 ? "click" : "clicks"} recorded</span>
      </div>

      <div className="profile-top">
        {isNeutral || top.length === 0 ? (
          <span className="profile-top-neutral">
            Exploring everything — click a card to shape it
          </span>
        ) : (
          <>
            <span className="profile-top-label">Top</span>
            <span className="profile-top-tags">
              {top.map((t) => (
                <span className="profile-top-chip" key={t}>
                  {t}
                </span>
              ))}
            </span>
          </>
        )}
      </div>

      <div className={isNeutral ? "profile-bars is-neutral" : "profile-bars"}>
        {sorted.map(({ tag, w }, i) => (
          <div
            className={
              !isNeutral && i === 0 && w > 0 ? "profile-bar-row is-top" : "profile-bar-row"
            }
            key={tag}
          >
            <span className="profile-bar-label">{tag}</span>
            <span className="profile-bar-track">
              <span className="profile-bar-fill" style={{ width: `${fillPct(w)}%` }} />
            </span>
            <span className="profile-bar-val">{fmt(w)}</span>
          </div>
        ))}
      </div>
    </div>
  );
}
