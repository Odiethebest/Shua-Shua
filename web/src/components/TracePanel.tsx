import { useState } from "react";
import type { TraceEntry } from "../engine";

// The collapsible DAG trace panel — the visible half of the project. Shows the
// cascade as a left-to-right funnel of stages (in -> out, latency, sample ids)
// and replays a staggered reveal each time the numbers change.
export default function TracePanel({ trace }: { trace: TraceEntry[] }) {
  const [expanded, setExpanded] = useState<boolean>(true);

  const topIn = trace.length > 0 ? trace[0].in : 0;
  const finalOut = trace.length > 0 ? trace[trace.length - 1].out : 0;
  const totalUs = trace.reduce((sum, e) => sum + e.latency_us, 0);

  // Changing this key remounts the flow so the CSS reveal animation replays.
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
        </span>
        <span className="trace-summary">
          {topIn.toLocaleString()} → {finalOut} · {trace.length} ops ·{" "}
          {totalUs.toFixed(0)}µs
          <span className="trace-chev">{expanded ? "▾" : "▸"}</span>
        </span>
      </button>

      {expanded && (
        <div className="trace-flow" key={flowKey}>
          {trace.map((e, i) => (
            <div className="trace-node" key={e.name}>
              <div className="trace-stage" style={{ animationDelay: `${i * 90}ms` }}>
                <div className="stage-name">{e.name}</div>
                <div className="stage-io">
                  <span className="io-in">{e.in.toLocaleString()}</span>
                  <span className="io-arrow">→</span>
                  <span className="io-out">{e.out.toLocaleString()}</span>
                </div>
                <div className="stage-lat">{e.latency_us.toFixed(1)}µs</div>
                <div className="stage-samples">
                  {e.sample_ids
                    .slice(0, 4)
                    .map((id) => `#${id}`)
                    .join("  ")}
                </div>
              </div>
              {i < trace.length - 1 && <span className="trace-arrow">▸</span>}
            </div>
          ))}
        </div>
      )}
    </section>
  );
}
