import { useState } from "react";
import { TAGS } from "../profile";

// Cold-start onboarding (v2 · B2): an optional tag picker shown on first visit.
// Selected tags seed the initial profile; skipping falls back to a neutral
// (diverse) profile. Either way the caller marks the profile onboarded.
export default function ColdStart({
  onFinish,
}: {
  onFinish: (tags: string[], remember: boolean) => void;
}) {
  const [selected, setSelected] = useState<Set<string>>(new Set());
  // "Remember me on this device" (v2 · B8). OFF by default → the profile lives in
  // sessionStorage and each launch starts fresh here at the picker; ON → localStorage,
  // so a return visit resumes this profile. Not a login — just how long state lives.
  const [remember, setRemember] = useState(false);

  const toggle = (tag: string) => {
    setSelected((prev) => {
      const next = new Set(prev);
      if (next.has(tag)) next.delete(tag);
      else next.add(tag);
      return next;
    });
  };

  return (
    <div className="coldstart">
      <div className="coldstart-card">
        <div className="coldstart-brand">
          <span className="brand-logo">刷</span>
          <span className="brand-name">
            Shua<span className="brand-accent">Shua</span>
          </span>
        </div>
        <h1 className="coldstart-title">What are you into?</h1>
        <p className="coldstart-sub">
          Pick a few interests to start — your feed learns as you click. It's
          optional; you can skip and explore everything.
        </p>

        <div className="coldstart-tags">
          {TAGS.map((tag) => (
            <button
              key={tag}
              type="button"
              className={selected.has(tag) ? "tag-chip is-selected" : "tag-chip"}
              aria-pressed={selected.has(tag)}
              onClick={() => toggle(tag)}
            >
              {tag}
            </button>
          ))}
        </div>

        <label className="coldstart-remember">
          <input
            type="checkbox"
            checked={remember}
            onChange={(e) => setRemember(e.target.checked)}
          />
          <span className="coldstart-remember-text">
            Remember me on this device
            <span className="coldstart-remember-hint">
              {remember ? "resumes on your next visit" : "off — starts fresh each launch"}
            </span>
          </span>
        </label>

        <div className="coldstart-actions">
          <button
            type="button"
            className="coldstart-continue"
            onClick={() => onFinish([...selected], remember)}
          >
            {selected.size > 0 ? `Continue with ${selected.size}` : "Continue"}
          </button>
          <button
            type="button"
            className="coldstart-skip"
            onClick={() => onFinish([], remember)}
          >
            Skip for now
          </button>
        </div>
      </div>
    </div>
  );
}
