import { TAGS, type Profile } from "../profile";

// The live profile panel (v2 · B3): the user's tag weights as bars. Clicks bump
// the weights (see recordClick), and because App holds the profile in state, this
// re-renders immediately — the bars grow and reorder in real time. Bar widths are
// normalized to the current max, so it shows *relative* interest.
export default function ProfilePanel({ profile }: { profile: Profile }) {
  const max = Math.max(1, ...TAGS.map((tag) => profile.tagWeights[tag] ?? 0));
  const sorted = [...TAGS].sort(
    (a, b) => (profile.tagWeights[b] ?? 0) - (profile.tagWeights[a] ?? 0),
  );

  return (
    <div className="profile-panel">
      <div className="profile-panel-head">
        <span className="profile-panel-title">🧠 Your profile</span>
        <span className="profile-panel-clicks">{profile.clickHistory.length} clicks</span>
      </div>
      <div className="profile-bars">
        {sorted.map((tag) => (
          <div className="profile-bar-row" key={tag}>
            <span className="profile-bar-label">{tag}</span>
            <span className="profile-bar-track">
              <span
                className="profile-bar-fill"
                style={{ width: `${((profile.tagWeights[tag] ?? 0) / max) * 100}%` }}
              />
            </span>
          </div>
        ))}
      </div>
    </div>
  );
}
