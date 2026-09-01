export interface CursorReadoutRow {
  key: string;
  label: string;
  color: string;
  formatted: string;
}

interface Props {
  timeSeconds: number;
  /** 0-100, the cursor's horizontal position within the well. */
  xPercent: number;
  rows: CursorReadoutRow[];
}

// Floating box anchored to the cursor, matching the well's legend order --
// audit #08/the Calibration frame's rule that a reader never has to remap
// which row is which channel. Flips to the cursor's left past the midpoint
// so it never runs off the well's right edge.
export function CursorReadout({ timeSeconds, xPercent, rows }: Props) {
  const anchorRight = xPercent > 55;
  return (
    <div
      className="cursor-readout"
      style={
        anchorRight
          ? { right: `${100 - xPercent}%`, marginRight: 10 }
          : { left: `${xPercent}%`, marginLeft: 10 }
      }
    >
      <div className="cursor-readout-time">t = {timeSeconds.toFixed(2)} s</div>
      <div className="cursor-readout-rule" />
      {rows.map((row) => (
        <div key={row.key} className="cursor-readout-row">
          <span className="cursor-readout-label">{row.label}</span>
          <span style={{ color: row.color }}>{row.formatted}</span>
        </div>
      ))}
    </div>
  );
}
