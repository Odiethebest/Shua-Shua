import { useEffect, useState } from "react";
import {
  getPersonas,
  recommend,
  type Persona,
  type Recommendation,
} from "./engine";
import Sidebar from "./components/Sidebar";
import TracePanel from "./components/TracePanel";
import Feed from "./components/Feed";

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

  // Apply + persist the theme.
  useEffect(() => {
    document.documentElement.setAttribute("data-theme", theme);
    localStorage.setItem("shua-theme", theme);
  }, [theme]);

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

  return (
    <div className="layout">
      <Sidebar
        personas={personas}
        activeId={activeId}
        onSelect={setActiveId}
        theme={theme}
        onToggleTheme={() => setTheme((t) => (t === "light" ? "dark" : "light"))}
      />
      <main className="main">
        <div className="main-inner">
          <header className="page-head">
            <h1 className="page-title">Explore</h1>
            <span className="page-sub">{rec !== null ? rec.persona : "…"}</span>
          </header>

          {error !== null && (
            <div className="notice error">Couldn&apos;t load the engine: {error}</div>
          )}

          {rec !== null && (
            <>
              <TracePanel trace={rec.trace} />
              <Feed items={rec.feed} />
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
