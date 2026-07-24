import type { Persona } from "../engine";
import { summarizeProfile, type Profile } from "../profile";

interface Props {
  personas: Persona[];
  activeId: number;
  onSelect: (id: number) => void;
  theme: "light" | "dark";
  onToggleTheme: () => void;
  profile: Profile;
}

// A small emoji per persona, from its label — mirrors Xiaohongshu's iconful nav.
function personaEmoji(label: string): string {
  const l = label.toLowerCase();
  if (l.includes("food") && l.includes("travel")) return "🧭";
  if (l.includes("food")) return "🍜";
  if (l.includes("travel")) return "✈️";
  if (l.includes("tech")) return "💻";
  if (l.includes("fashion")) return "👗";
  if (l.includes("fit")) return "🏋️";
  if (l.includes("beaut")) return "💄";
  return "✨";
}

// Left sidebar: brand, the persona switcher (Xiaohongshu-style nav), and a
// light/dark theme toggle.
export default function Sidebar({
  personas,
  activeId,
  onSelect,
  theme,
  onToggleTheme,
  profile,
}: Props) {
  return (
    <aside className="sidebar">
      <div className="brand">
        <span className="brand-logo">刷</span>
        <span className="brand-name">
          Shua<span className="brand-accent">Shua</span>
        </span>
      </div>
      <p className="brand-tagline">a C++ recommendation engine, running in your browser</p>

      <nav className="nav" aria-label="persona">
        <div className="nav-label">Personas</div>
        {personas.map((p) => (
          <button
            key={p.id}
            type="button"
            className={p.id === activeId ? "nav-item is-active" : "nav-item"}
            onClick={() => onSelect(p.id)}
          >
            <span className="nav-emoji">{personaEmoji(p.label)}</span>
            <span className="nav-text">{p.label}</span>
          </button>
        ))}
      </nav>

      <div className="sidebar-foot">
        <div className="profile-hint">🧠 profile: {summarizeProfile(profile)}</div>
        <button type="button" className="theme-toggle" onClick={onToggleTheme}>
          <span>{theme === "light" ? "🌙" : "☀️"}</span>
          <span>{theme === "light" ? "Dark" : "Light"} mode</span>
        </button>
        <div className="engine-badge">engine: C++ → WASM</div>
      </div>
    </aside>
  );
}
