import { TAGS, type Profile } from "../profile";
import ProfilePanel from "./ProfilePanel";

interface Props {
  theme: "light" | "dark";
  onToggleTheme: () => void;
  profile: Profile;
  onReset: () => void;
  remembered: boolean;
}

// Left sidebar: brand, the live profile panel (v2 — this replaces v1's persona
// switcher), a "start over" reset, a small state-aware coaching line, and a footer
// with a session-state note, the light/dark toggle, and a quiet technical signature.
export default function Sidebar({ theme, onToggleTheme, profile, onReset, remembered }: Props) {
  const clicks = profile.clickHistory.length;
  // Top interest so far — only surfaced in the coaching line once the user has clicked.
  const topTag = [...TAGS].sort(
    (a, b) => (profile.tagWeights[b] ?? 0) - (profile.tagWeights[a] ?? 0),
  )[0];
  const hint =
    clicks === 0
      ? "Click notes you like — I'll learn your taste."
      : `Your taste leans ${topTag} — hit Refresh for updated picks.`;

  return (
    <aside className="sidebar">
      <div className="brand">
        <span className="brand-logo">刷</span>
        <span className="brand-name">
          Shua<span className="brand-accent">Shua</span>
        </span>
      </div>
      <p className="brand-tagline">a C++ recommendation engine, running in your browser</p>

      <ProfilePanel profile={profile} />
      <button type="button" className="profile-reset" onClick={onReset}>
        ↺ Start over
      </button>
      <p className="sidebar-hint">{hint}</p>

      <div className="sidebar-foot">
        <div className={remembered ? "session-state is-remembered" : "session-state"}>
          <span className="session-dot" />
          <span>{remembered ? "Remembered on this device" : "This session won't be saved"}</span>
        </div>
        <button type="button" className="theme-toggle" onClick={onToggleTheme}>
          <span>{theme === "light" ? "🌙" : "☀️"}</span>
          <span>{theme === "light" ? "Dark" : "Light"} mode</span>
        </button>
        <div className="sidebar-sig">
          <div className="engine-badge">engine: C++ → WASM</div>
          <div className="engine-badge">3,000 notes · in-memory · sub-ms recall</div>
        </div>
      </div>
    </aside>
  );
}
