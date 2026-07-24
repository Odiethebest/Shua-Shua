import type { Profile } from "../profile";
import ProfilePanel from "./ProfilePanel";

interface Props {
  theme: "light" | "dark";
  onToggleTheme: () => void;
  profile: Profile;
}

// Left sidebar: brand, the live profile panel (v2 — this replaces v1's persona
// switcher, since the feed is now driven by the profile, not a picked persona), and
// a light/dark theme toggle.
export default function Sidebar({ theme, onToggleTheme, profile }: Props) {
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

      <div className="sidebar-foot">
        <button type="button" className="theme-toggle" onClick={onToggleTheme}>
          <span>{theme === "light" ? "🌙" : "☀️"}</span>
          <span>{theme === "light" ? "Dark" : "Light"} mode</span>
        </button>
        <div className="engine-badge">engine: C++ → WASM</div>
      </div>
    </aside>
  );
}
