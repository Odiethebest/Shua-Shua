import { useEffect, useState } from "react";
import {
  getPersonas,
  recommend,
  recommendSimilar,
  type Persona,
  type Recommendation,
} from "./engine";
import Sidebar from "./components/Sidebar";
import TracePanel from "./components/TracePanel";
import Feed from "./components/Feed";
import { loadProfile, saveProfile, type Profile } from "./profile";

type Theme = "light" | "dark";

function initialTheme(): Theme {
  // The pre-paint script in index.html already resolved this onto <html>.
  return document.documentElement.getAttribute("data-theme") === "dark" ? "dark" : "light";
}

export default function App() {
  const [personas, setPersonas] = useState<Persona[]>([]);
  const [activeId, setActiveId] = useState<number>(2); // default: "Foodie + Traveler"
  const [rec, setRec] = useState<Recommendation | null>(null);
  const [loading, setLoading] = useState<boolean>(true);
  const [error, setError] = useState<string | null>(null);
  const [theme, setTheme] = useState<Theme>(initialTheme);
  const [focusTitle, setFocusTitle] = useState<string | null>(null); // set when a card is clicked
  // v2 behavior-driven profile (B1: loaded from local storage, persisted below).
  // Later blocks seed it (B2), grow it from clicks (B3), and drive the feed (B5).
  const [profile] = useState<Profile>(() => loadProfile());

  // Apply + persist the theme.
  useEffect(() => {
    document.documentElement.setAttribute("data-theme", theme);
    localStorage.setItem("shua-theme", theme);
  }, [theme]);

  // Persist the profile whenever it changes (B1). It is static this block; B2/B3
  // start mutating it, and this effect keeps local storage in sync.
  useEffect(() => {
    saveProfile(profile);
  }, [profile]);

  // Load the persona list once.
  useEffect(() => {
    getPersonas()
      .then(setPersonas)
      .catch((e: unknown) => setError(String(e)));
  }, []);

  // Re-run the pipeline whenever the selected persona changes.
  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    setFocusTitle(null); // a persona query, not an item-based one
    recommend(activeId)
      .then((r) => {
        if (!cancelled) {
          setRec(r);
          setLoading(false);
        }
      })
      .catch((e: unknown) => {
        if (!cancelled) {
          setError(String(e));
          setLoading(false);
        }
      });
    return () => {
      cancelled = true;
    };
  }, [activeId]);

  // Clicking a card re-runs the engine with THAT item's own vector as the query
  // ("more like this"), shifting the feed toward its neighborhood.
  const openItem = (id: number, title: string) => {
    setLoading(true);
    recommendSimilar(id)
      .then((r) => {
        setRec(r);
        setFocusTitle(title);
        setLoading(false);
      })
      .catch((e: unknown) => {
        setError(String(e));
        setLoading(false);
      });
  };

  return (
    <div className="layout">
      <Sidebar
        personas={personas}
        activeId={activeId}
        onSelect={setActiveId}
        theme={theme}
        onToggleTheme={() => setTheme((t) => (t === "light" ? "dark" : "light"))}
        profile={profile}
      />
      <main className="main">
        <div className="main-inner">
          <header className="page-head">
            <h1 className="page-title">Explore</h1>
            <span className="page-sub">
              {focusTitle !== null
                ? `More like: ${focusTitle}`
                : rec !== null
                  ? rec.persona
                  : "…"}
            </span>
          </header>

          {error !== null && (
            <div className="notice error">Couldn&apos;t load the engine: {error}</div>
          )}

          {rec !== null && (
            <>
              <TracePanel trace={rec.trace} />
              <Feed items={rec.feed} onOpenItem={openItem} />
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
