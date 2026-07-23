import { useEffect, useState } from "react";
import {
  getPersonas,
  recommend,
  type Persona,
  type Recommendation,
} from "./engine";
import PersonaTabs from "./components/PersonaTabs";
import TracePanel from "./components/TracePanel";
import Feed from "./components/Feed";

export default function App() {
  const [personas, setPersonas] = useState<Persona[]>([]);
  const [activeId, setActiveId] = useState<number>(2); // default: "Foodie + Traveler"
  const [rec, setRec] = useState<Recommendation | null>(null);
  const [loading, setLoading] = useState<boolean>(true);
  const [error, setError] = useState<string | null>(null);

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
    <div className="app">
      <div className="header">
        <header className="topbar">
          <div className="brand">
            Shua<span className="brand-accent">Shua</span>
          </div>
          <div className="tagline">
            a C++ recommendation engine, running in your browser
          </div>
        </header>
        <PersonaTabs personas={personas} activeId={activeId} onSelect={setActiveId} />
      </div>

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
        <div className="notice loading">running the pipeline…</div>
      )}
    </div>
  );
}
