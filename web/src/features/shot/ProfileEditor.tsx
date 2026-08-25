import { useId, useState } from "react";

import type { ProfilePoint } from "../../api/types";
import { ProfileCanvas } from "./ProfileCanvas";

interface Props {
  points: ProfilePoint[];
  range: readonly [number, number];
  maxTimeSeconds: number;
  unit: string;
  color: string;
  onChange: (points: ProfilePoint[]) => void;
}

// Section 12.3: a graphical control over the same points, with direct numeric
// editing underneath. The numeric form is the one that survives a scope cut
// (15.2), so it stays first-class rather than becoming a read-out.
export function ProfileEditor({ points, range, maxTimeSeconds, unit, color, onChange }: Props) {
  const [showNumbers, setShowNumbers] = useState(false);
  const numericPointsId = useId();
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
      <ProfileCanvas
        points={points}
        range={range}
        maxTimeSeconds={maxTimeSeconds}
        unit={unit}
        color={color}
        onChange={onChange}
      />

      <button
        className="ghost"
        style={{ marginTop: 6, fontSize: 11, padding: "4px 8px" }}
        onClick={() => setShowNumbers((current) => !current)}
        aria-expanded={showNumbers}
        aria-controls={numericPointsId}
      >
        {showNumbers ? "Hide" : "Edit"} numeric points ({points.length})
      </button>

      {showNumbers && (
        <div id={numericPointsId} style={{ marginTop: 8 }}>
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
                onChange={(e) => update(index, 0, Number(e.target.value))}
              />
              <input
                type="number" step={0.5} min={range[0]} max={range[1]} value={point[1]}
                aria-label={`Point ${index + 1} value in ${unit}`}
                onChange={(e) => update(index, 1, Number(e.target.value))}
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
          <button
            className="ghost"
            aria-label="Add profile point"
            style={{ marginTop: 4, fontSize: 12 }}
            onClick={add}
          >
            Add point
          </button>
        </div>
      )}
    </div>
  );
}
