import { useEffect, useState } from "react";
import { recommendFromProfile, type Recommendation } from "./engine";
import Sidebar from "./components/Sidebar";
import TracePanel from "./components/TracePanel";
import Feed from "./components/Feed";
import ColdStart from "./components/ColdStart";
import {
  categoryWeights,
  decayProfile,
  loadProfile,
  NEW_RATIO,
  recordClick,
  saveProfile,
  seededProfile,
  type Profile,
} from "./profile";

type Theme = "light" | "dark";

function initialTheme(): Theme {
  // The pre-paint script in index.html already resolved this onto <html>.
  return document.documentElement.getAttribute("data-theme") === "dark" ? "dark" : "light";
}

export default function App() {
  const [rec, setRec] = useState<Recommendation | null>(null);
  const [loading, setLoading] = useState<boolean>(true);
  const [error, setError] = useState<string | null>(null);
  const [theme, setTheme] = useState<Theme>(initialTheme);
  // v2 behavior-driven profile: loaded from local storage, persisted on change,
  // seeded at cold start (B2), grown by clicks (B3), decayed (B4), and — from B5 —
  // the source of the recall query (replacing v1's personas).
  const [profile, setProfile] = useState<Profile>(() => loadProfile());

  // Apply + persist the theme.
  useEffect(() => {
    document.documentElement.setAttribute("data-theme", theme);
    localStorage.setItem("shua-theme", theme);
  }, [theme]);

  // Persist the profile whenever it changes.
  useEffect(() => {
    saveProfile(profile);
  }, [profile]);

  // Build the feed FROM a profile (v2 · B5): collapse tag→category weights and run
  // the DAG against the profile vector the engine builds from them. This is the
  // recall query now. Kept as a plain function (not an effect) because the feed
  // must re-run only on explicit events — the initial load and, later, the refresh
  // button (B6) — never on every click (B3).
  const runFeed = (p: Profile) => {
    setLoading(true);
    recommendFromProfile(categoryWeights(p), [...p.seenItemIds], NEW_RATIO)
      .then((r) => {
        setRec(r);
        setLoading(false);
      })
      .catch((e: unknown) => {
        setError(String(e));
        setLoading(false);
      });
  };

  // Initial feed on mount for a returning (already-onboarded) user. A new user's
  // first feed runs when they finish onboarding (finishOnboarding).
  useEffect(() => {
    if (profile.onboarded) runFeed(profile);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []); // mount only

  // Clicking a card is implicit feedback (B3): it bumps the profile in real time
  // (the live panel grows). The FEED does not move — it re-ranks only on refresh.
  const handleCardClick = (id: number, category: string) => {
    setProfile((p) => recordClick(p, id, category));
  };

  // Cold start (B2): seed the profile from the picker, then run the first feed
  // from that seeded profile (§3 step 2).
  const finishOnboarding = (tags: string[]) => {
    const seeded = seededProfile(tags);
    setProfile(seeded);
    runFeed(seeded);
  };

  // "Refresh recommendations" (B6): age the profile one decay step (B4's decay is
  // triggered HERE now) and re-rank the feed against it. The engine's new/seen mix
  // keeps the refresh from just replaying already-clicked items — it returns mostly
  // new content with a small quota of proven favorites (exploration/exploitation).
  const handleRefresh = () => {
    const decayed = decayProfile(profile);
    setProfile(decayed);
    runFeed(decayed);
  };

  if (!profile.onboarded) {
    return <ColdStart onFinish={finishOnboarding} />;
  }

  return (
    <div className="layout">
      <Sidebar
        theme={theme}
        onToggleTheme={() => setTheme((t) => (t === "light" ? "dark" : "light"))}
        profile={profile}
      />
      <main className="main">
        <div className="main-inner">
          <header className="page-head">
            <div className="page-head-titles">
              <h1 className="page-title">Explore</h1>
              <span className="page-sub">{rec !== null ? rec.persona : "…"}</span>
            </div>
            <button
              type="button"
              className="refresh-btn"
              onClick={handleRefresh}
              disabled={loading || rec === null}
            >
              ↻ Refresh recommendations
            </button>
          </header>

          {error !== null && (
            <div className="notice error">Couldn&apos;t load the engine: {error}</div>
          )}

          {rec !== null && (
            <>
              <TracePanel trace={rec.trace} />
              <Feed items={rec.feed} onCardClick={handleCardClick} />
            </>
          )}

          {loading && rec === null && error === null && (
            <div className="notice">running the pipeline…</div>
          )}
        </div>
      </main>
    </div>
  );
}
