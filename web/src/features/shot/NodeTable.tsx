import type { ProfilePoint } from "../../api/types";

interface Props {
  points: ProfilePoint[];
  range: readonly [number, number];
  unit: string;
  onUpdate: (index: number, position: 0 | 1, value: number) => void;
  onAdd: () => void;
  onChange: (points: ProfilePoint[]) => void;
}

// The always-visible numeric half of ProfileEditor (audit #08). Tabular
// figures, one row per node, so a mistyped value is visible as a broken
// column rather than hidden behind a toggle.
export function NodeTable({ points, range, unit, onUpdate, onAdd, onChange }: Props) {
  return (
    <div className="node-table">
      <div className="profile-row">
        <span className="note" style={{ flex: 1 }}>time (s)</span>
        <span className="note" style={{ flex: 1 }}>value</span>
        <span style={{ width: 18 }} />
      </div>
      {points.map((point, index) => (
        <div className="profile-row" key={index}>
          <input
            type="number" step={0.5} value={point[0]}
            aria-label={`Point ${index + 1} time in seconds`}
            onChange={(e) => onUpdate(index, 0, Number(e.target.value))}
          />
          <input
            type="number" step={0.5} min={range[0]} max={range[1]} value={point[1]}
            aria-label={`Point ${index + 1} value in ${unit}`}
            onChange={(e) => onUpdate(index, 1, Number(e.target.value))}
          />
          <button
            className="x"
            title="remove point"
            aria-label={`Remove point ${index + 1}`}
            disabled={points.length <= 1}
            onClick={() => onChange(points.filter((_, i) => i !== index))}
          >
            ×
          </button>
        </div>
      ))}
      <button className="ghost" aria-label="Add profile point" style={{ marginTop: 4, fontSize: 12 }} onClick={onAdd}>
        Add point
      </button>
    </div>
  );
}
