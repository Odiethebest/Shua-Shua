import { useState } from "react";
import type { TraceEntry } from "../engine";

// The collapsible DAG trace panel — the visible half of the project. It renders the
// cascade as a left-to-right FUNNEL: one column per operator, with a faint "in" bar
// behind a solid "out" bar, so both the per-op narrowing and the overall
// 3,000 → 300 → 50 → 24 → 12 shape read at a glance. It names the profile that drove
// the run and replays a staggered reveal + bar flash each time the shape changes.
export default function TracePanel({
  trace,
  drivenBy,
}: {
  trace: TraceEntry[];
  drivenBy?: string;
}) {
  const [expanded, setExpanded] = useState<boolean>(true);

  const topIn = trace.length > 0 ? trace[0].in : 0;
  const finalOut = trace.length > 0 ? trace[trace.length - 1].out : 0;
  const totalUs = trace.reduce((sum, e) => sum + e.latency_us, 0);

  // Browsers coarsen performance.now() (the engine's timer) to ~0 unless the page is
  // cross-origin isolated (COOP + COEP). When it isn't, say so, so a 0.0µs reads as
  // "timer clamped," not "the C++ was instant."
  const timersSharp = typeof window !== "undefined" && window.crossOriginIsolated;

  // Bar height is a LOG scale of the count: the funnel spans a huge range (pool ≈ 3,000
  // down to a 12-card page), so a linear scale would make the small stages invisible.
  // maxN is the funnel mouth (the pool RecallOp scans).
  const maxN = Math.max(1, ...trace.map((e) => e.in));
  const heightPct = (n: number) =>
    Math.max(8, (Math.log(Math.max(1, n)) / Math.log(maxN)) * 100);

  // Changing this key remounts the funnel so the reveal/flash animation replays when
  // the numbers change (e.g. on refresh, or when the pipeline grows a MixOp stage).
  const flowKey = trace.map((e) => `${e.name}:${e.out}`).join("|");

  return (
    <section className="trace">
      <button
        type="button"
        className="trace-header"
        onClick={() => setExpanded((v) => !v)}
        aria-expanded={expanded}
      >
        <span className="trace-title">
          <span className="trace-dot" />
          DAG pipeline trace
          {drivenBy !== undefined && drivenBy !== "" && (
            <span className="trace-driver">driven by {drivenBy}</span>
          )}
        </span>
        <span className="trace-summary">
          {topIn.toLocaleString()} → {finalOut} · {trace.length} ops ·{" "}
          {totalUs.toFixed(0)}µs
          <span className="trace-chev">{expanded ? "▾" : "▸"}</span>
        </span>
      </button>

      {expanded && (
        <>
          <div className="pipe" key={flowKey}>
            {trace.map((e, i) => (
              <div className="pipe-node" key={e.name}>
                <div
                  className="pipe-col"
                  style={{ animationDelay: `${i * 90}ms` }}
                  title={
                    e.sample_ids.length > 0
                      ? `sample output ids: ${e.sample_ids.map((id) => `#${id}`).join("  ")}`
                      : undefined
                  }
                >
                  <div className="pipe-track">
                    <span className="pipe-bar-in" style={{ height: `${heightPct(e.in)}%` }} />
                    <span className="pipe-bar-out" style={{ height: `${heightPct(e.out)}%` }} />
                    <span className="pipe-count" style={{ bottom: `${heightPct(e.out)}%` }}>
                      {e.out.toLocaleString()}
                    </span>
                  </div>
                  <div className="pipe-name">{e.name}</div>
                  <div className="pipe-io">
                    {e.in.toLocaleString()} <span className="pipe-io-arrow">→</span>{" "}
                    {e.out.toLocaleString()}
                  </div>
                  <div className="pipe-lat">{e.latency_us.toFixed(1)}µs</div>
                  {e.detail !== "" && <div className="pipe-detail">{e.detail}</div>}
                </div>
                {i < trace.length - 1 && <span className="pipe-arrow">▸</span>}
              </div>
            ))}
          </div>
          {!timersSharp && (
            <div className="trace-note">
              latencies read ~0µs here — the browser coarsens its timer unless the page
              is cross-origin isolated (COOP + COEP headers)
            </div>
          )}
        </>
      )}
    </section>
  );
}
