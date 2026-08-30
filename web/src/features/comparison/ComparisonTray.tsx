import type { ShotResult } from "../../api/types";

interface Props {
  runs: ShotResult[];
  activeId?: string;
  onRemove: (runId: string) => void;
  onPin: () => void;
  canPin: boolean;
}

// Two or three runs may overlay; unlimited comparison is explicitly unreadable
// and is not offered (12.5).
const MAX_COMPARISONS = 2;

export function ComparisonTray({ runs, activeId, onRemove, onPin, canPin }: Props) {
  return (
    <div className="chart-card">
      <h3>Comparison</h3>
      <div className="row" style={{ flexWrap: "wrap" }}>
        <button className="ghost" onClick={onPin} disabled={!canPin || runs.length >= MAX_COMPARISONS}>
          Pin current run
        </button>
        {runs.length === 0 && (
          <span className="note">
            Pin up to {MAX_COMPARISONS} runs to overlay them on the charts above.
          </span>
        )}
        {runs.map((run) => (
          <span key={run.manifest.run_id} className="metric" style={{ minWidth: 0, padding: "6px 10px" }}>
            <span className="label">{run.manifest.run_id.slice(5, 11)}</span>
            <span style={{ fontSize: 12 }}>
              {run.elapsed_time_s.toFixed(1)} s · {run.extraction_yield_percent.toFixed(1)} %
            </span>
            <button
              className="x" style={{ marginLeft: 6, background: "none", border: "none",
                                     color: "var(--muted)", cursor: "pointer" }}
              aria-label={`Remove pinned run ${run.manifest.run_id} from comparison`}
              onClick={() => onRemove(run.manifest.run_id)}
            >
              ×
            </button>
          </span>
        ))}
      </div>
      {activeId && runs.some((run) => run.manifest.run_id === activeId) && (
        <p className="note" style={{ marginTop: 6 }}>
          The current run is already pinned; it is drawn solid and its overlay is dashed.
        </p>
      )}
    </div>
  );
}
