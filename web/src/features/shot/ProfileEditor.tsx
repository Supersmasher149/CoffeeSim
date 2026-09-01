import type { ProfilePoint } from "../../api/types";
import { NodeTable } from "./NodeTable";
import { ProfileCanvas } from "./ProfileCanvas";

interface Props {
  points: ProfilePoint[];
  range: readonly [number, number];
  maxTimeSeconds: number;
  unit: string;
  color: string;
  onChange: (points: ProfilePoint[]) => void;
}

// Audit #08: the numeric point list used to default to hidden behind an 11px
// ghost toggle, so the graph and the numbers were never visible together.
// The node table is now always rendered beside/below the graph -- the one
// view this system treats as authoritative on a scope cut (15.2) is now also
// the one always on screen.
export function ProfileEditor({ points, range, maxTimeSeconds, unit, color, onChange }: Props) {
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
    <div className="profile-editor">
      <ProfileCanvas
        points={points}
        range={range}
        maxTimeSeconds={maxTimeSeconds}
        unit={unit}
        color={color}
        onChange={onChange}
      />
      <NodeTable points={points} range={range} unit={unit} onUpdate={update} onAdd={add} onChange={onChange} />
    </div>
  );
}
