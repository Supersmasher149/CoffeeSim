import type { ProfilePoint } from "../../api/types";

interface Props {
  points: ProfilePoint[];
  range: readonly [number, number];
  onChange: (points: ProfilePoint[]) => void;
}

// Direct numeric editing of the time/value points (12.3). A graphical editor is
// the first thing cut if the schedule slips (15.2), so the numeric form is the
// one that has to work.
export function ProfileEditor({ points, range, onChange }: Props) {
  const update = (index: number, position: 0 | 1, value: number) => {
    const next = points.map((point, i) =>
      i === index ? ((position === 0 ? [value, point[1]] : [point[0], value]) as ProfilePoint) : point,
    );
    onChange(next);
  };

  const add = () => {
    const last = points[points.length - 1];
    onChange([...points, [last ? last[0] + 5 : 0, last ? last[1] : range[0]]]);
  };

  return (
    <div>
      <div className="profile-row">
        <span className="note" style={{ flex: 1 }}>time (s)</span>
        <span className="note" style={{ flex: 1 }}>value</span>
        <span style={{ width: 18 }} />
      </div>
      {points.map((point, index) => (
        <div className="profile-row" key={index}>
          <input
            type="number" step={0.5} value={point[0]}
            onChange={(e) => update(index, 0, Number(e.target.value))}
          />
          <input
            type="number" step={0.5} min={range[0]} max={range[1]} value={point[1]}
            onChange={(e) => update(index, 1, Number(e.target.value))}
          />
          <button
            className="x" title="remove point" disabled={points.length <= 1}
            onClick={() => onChange(points.filter((_, i) => i !== index))}
          >
            ×
          </button>
        </div>
      ))}
      <button className="ghost" style={{ marginTop: 4, fontSize: 12 }} onClick={add}>
        Add point
      </button>
    </div>
  );
}
