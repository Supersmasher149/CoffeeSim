import { Fragment, useRef, useState } from "react";

import type { SweepRunRow } from "../../api/types";

export type HeatMetric = "shot_time_s" | "extraction_yield_percent" | "tds_percent" | "beverage_mass_g";

interface Props {
  rows: SweepRunRow[];
  xValues: number[];
  yValues: number[];
  xLabel: string;
  yLabel: string;
  metric: HeatMetric;
  metricLabel: string;
  metricUnit: string;
  partial?: boolean;
  /** The baseline recipe's current value for each axis, if that axis maps to
   * a resolvable scalar (audit #05) -- marks which cell the sweep varied
   * away from and lets the detail panel report a delta. */
  baselineY?: number;
  baselineX?: number;
}

// Sequential, one hue, light to dark - the color job for magnitude on a grid.
// The dark-mode anchor is flipped: low values sit just above the panel surface
// and high values are the brightest step. Lightness is monotonic across the
// ramp and the hue spread is 20 degrees, so it reads as one scale.
const RAMP = ["#241c17", "#4a2f1b", "#7a4a22", "#a8662c", "#d1893f", "#edb571", "#f8dcb4"];

// Axis values come from a linear spread and can carry a long decimal tail
// (94.794872). Round for display only; the sweep keeps the exact value.
function formatTick(value: number): string {
  if (Number.isInteger(value)) return String(value);
  const rounded = Math.abs(value) >= 100 ? value.toFixed(0) : value.toFixed(1);
  return String(Number(rounded));
}

// At 40 steps a label per cell is an unreadable smear, so show at most a dozen
// and let the hover carry the exact coordinates.
function tickStride(count: number): number {
  return Math.max(1, Math.ceil(count / 12));
}

function rampColor(t: number): string {
  if (!Number.isFinite(t)) return RAMP[0];
  const clamped = Math.min(Math.max(t, 0), 1);
  const scaled = clamped * (RAMP.length - 1);
  const index = Math.min(Math.floor(scaled), RAMP.length - 2);
  const f = scaled - index;

  const lerp = (a: string, b: string) => {
    const channel = (offset: number) =>
      Math.round(
        parseInt(a.slice(offset, offset + 2), 16) * (1 - f) +
          parseInt(b.slice(offset, offset + 2), 16) * f,
      );
    return `rgb(${channel(1)}, ${channel(3)}, ${channel(5)})`;
  };
  return lerp(RAMP[index], RAMP[index + 1]);
}

// Extraction yield deliberately gets the same sequential ramp as everything
// else. A diverging scale centred on "ideal extraction" would encode a flavour
// judgement the model does not support (section 8.5). That stays true now that a
// per-bean sensory overlay exists: the overlay is bean-relative and lives on the
// shot view, and no sweep metric encodes a taste judgement.
// Audit #09: a single `hovered` slot used to reset on blur, so the detail
// panel fell back to a placeholder and two cells could never be compared
// side by side. `preview` keeps the old live hover/focus scan; `pinned` is
// set by clicking (or activating with Enter/Space, since these are real
// buttons) and survives blur; shift-click sets a second `held` cell so two
// runs can sit in the detail panel together.
export function HeatMap({
  rows, xValues, yValues, xLabel, yLabel, metric, metricLabel, metricUnit, partial = false,
  baselineY, baselineX,
}: Props) {
  const [preview, setPreview] = useState<SweepRunRow>();
  const [pinned, setPinned] = useState<SweepRunRow>();
  const [held, setHeld] = useState<SweepRunRow>();
  const cellRefs = useRef(new Map<string, HTMLButtonElement>());

  const primary = preview ?? pinned;

  const byCoordinate = new Map<string, SweepRunRow>();
  for (const row of rows) byCoordinate.set(`${row.coordinates[0]}|${row.coordinates[1]}`, row);

  const valid = rows.filter((row) => row.termination !== "invalid_state");
  const values = valid.map((row) => row[metric]);
  const min = values.length ? Math.min(...values) : 0;
  const max = values.length ? Math.max(...values) : 1;
  const span = max - min || 1;

  const format = (value: number) => value.toFixed(metric === "shot_time_s" ? 1 : 2);

  // Rows get thinner as the grid grows so a large sweep still fits on screen.
  const cellHeight = yValues.length > 24 ? 13 : yValues.length > 14 ? 19 : 26;
  const xStride = tickStride(xValues.length);
  const yStride = tickStride(yValues.length);
  const reversedY = [...yValues].reverse();

  return (
    <div
      role="group"
      aria-label={
        `Heat map of ${metricLabel} across ${xLabel} and ${yLabel}, ` +
        `ranging ${format(min)} to ${format(max)} ${metricUnit}. ` +
        "Tab or use arrow keys to move between cells; click or press Enter to pin one's detail, " +
        "shift-click a second cell to hold it alongside for comparison."
      }
    >
      <div style={{ display: "flex", gap: 10 }}>
        <div
          style={{
            writingMode: "vertical-rl",
            transform: "rotate(180deg)",
            fontSize: 11,
            color: "var(--muted)",
            textAlign: "center",
            paddingBottom: 22,
          }}
        >
          {yLabel}
        </div>

        <div style={{ flex: 1, minWidth: 0, overflowX: "auto" }}>
          <div
            style={{
              display: "grid",
              gridTemplateColumns: `48px repeat(${xValues.length}, minmax(8px, 1fr))`,
              gap: xValues.length > 24 ? 1 : 2,
            }}
          >
            {/* Rows run top-down from the highest y value, so the grid reads
                like a plot rather than a spreadsheet. */}
            {reversedY.map((y, rowIndex) => (
              <Fragment key={`row-${y}`}>
                <div
                  style={{
                    fontSize: 10,
                    color: "var(--muted)",
                    display: "flex",
                    alignItems: "center",
                    justifyContent: "flex-end",
                    paddingRight: 4,
                    fontFamily: "ui-monospace, Menlo, monospace",
                  }}
                >
                  {rowIndex % yStride === 0 ? formatTick(y) : ""}
                </div>
                {xValues.map((x, columnIndex) => {
                  const row = byCoordinate.get(`${y}|${x}`);
                  const invalid = !row || row.termination === "invalid_state";
                  const t = row ? (row[metric] - min) / span : 0;
                  const isBaseline =
                    baselineY !== undefined &&
                    baselineX !== undefined &&
                    Math.abs(y - baselineY) < 1e-6 &&
                    Math.abs(x - baselineX) < 1e-6;
                  // A hover-only tooltip leaves color as the only carrier of
                  // the value and is unreachable from the keyboard. Each cell
                  // is a real button: focus/blur mirror the hover handlers so
                  // Tab reaches the same detail panel a mouse does, and the
                  // aria-label is the non-color alternative to the ramp.
                  const label =
                    (!row
                      ? `${yLabel} ${formatTick(y)}, ${xLabel} ${formatTick(x)}: ` +
                        (partial ? "not run before cancellation" : "no run")
                      : invalid
                        ? `${yLabel} ${formatTick(y)}, ${xLabel} ${formatTick(x)}: outside the supported input range`
                        : `${yLabel} ${formatTick(y)}, ${xLabel} ${formatTick(x)}: ` +
                          `${metricLabel} ${format(row[metric])} ${metricUnit}, ${row.termination.replace(/_/g, " ")}`) +
                    (isBaseline ? " · baseline" : "");
                  const cellKey = `${rowIndex}-${columnIndex}`;
                  return (
                    <button
                      key={`${y}-${x}`}
                      ref={(el) => {
                        if (el) cellRefs.current.set(cellKey, el);
                        else cellRefs.current.delete(cellKey);
                      }}
                      type="button"
                      disabled={!row}
                      aria-label={label}
                      aria-pressed={pinned?.index === row?.index}
                      onMouseEnter={() => setPreview(row)}
                      onMouseLeave={() => setPreview(undefined)}
                      onFocus={() => setPreview(row)}
                      onBlur={() => setPreview(undefined)}
                      onClick={(event) => {
                        if (!row) return;
                        if (event.shiftKey) {
                          setHeld((current) => (current?.index === row.index ? undefined : row));
                        } else {
                          setPinned(row);
                        }
                      }}
                      onKeyDown={(event) => {
                        const deltas: Record<string, [number, number]> = {
                          ArrowUp: [-1, 0], ArrowDown: [1, 0], ArrowLeft: [0, -1], ArrowRight: [0, 1],
                        };
                        const delta = deltas[event.key];
                        if (!delta) return;
                        event.preventDefault();
                        const target = cellRefs.current.get(`${rowIndex + delta[0]}-${columnIndex + delta[1]}`);
                        target?.focus();
                      }}
                      style={{
                        height: cellHeight,
                        borderRadius: 2,
                        border: "none",
                        padding: 0,
                        cursor: row ? "pointer" : "default",
                        // Invalid corners are a state, not a magnitude, so they
                        // leave the ramp entirely and carry a texture.
                         background: !row && partial
                           ? "repeating-linear-gradient(135deg, #273137 0 4px, #182126 4px 8px)"
                           : invalid
                           ? "repeating-linear-gradient(45deg, #2b2320 0 4px, #1e1917 4px 8px)"
                          : rampColor(t),
                        outline:
                          row && (primary?.index === row.index || pinned?.index === row.index)
                            ? "2px solid var(--text)"
                            : row && held?.index === row.index
                              ? "2px dashed var(--accent)"
                              : "none",
                        outlineOffset: -2,
                        boxShadow: isBaseline ? "inset 0 0 0 1px var(--text)" : undefined,
                      }}
                    />
                  );
                })}
              </Fragment>
            ))}

            <div />
            {xValues.map((x, columnIndex) => (
              <div
                key={`x-${x}`}
                style={{
                  fontSize: 10,
                  color: "var(--muted)",
                  textAlign: "center",
                  paddingTop: 4,
                  fontFamily: "ui-monospace, Menlo, monospace",
                  whiteSpace: "nowrap",
                  overflow: "visible",
                }}
              >
                {columnIndex % xStride === 0 ? formatTick(x) : ""}
              </div>
            ))}
          </div>
          <div style={{ fontSize: 11, color: "var(--muted)", textAlign: "center", marginTop: 4 }}>
            {xLabel}
          </div>
        </div>
      </div>

      <div className="row" style={{ marginTop: 10, gap: 14, flexWrap: "wrap" }}>
        <div className="row" style={{ gap: 6 }}>
          <span className="note">
            {metricLabel} ({metricUnit})
          </span>
          <span className="note" style={{ fontFamily: "ui-monospace, Menlo, monospace" }}>
            {format(min)}
          </span>
          <div
            style={{
              width: 130,
              height: 10,
              borderRadius: 2,
              background: `linear-gradient(to right, ${RAMP.join(", ")})`,
            }}
          />
          <span className="note" style={{ fontFamily: "ui-monospace, Menlo, monospace" }}>
            {format(max)}
          </span>
        </div>
        <div className="row" style={{ gap: 6 }}>
          <div
            style={{
              width: 16,
              height: 10,
              borderRadius: 2,
              background: "repeating-linear-gradient(45deg, #2b2320 0 4px, #1e1917 4px 8px)",
            }}
          />
          <span className="note">outside the supported input range</span>
        </div>
        {partial && (
          <div className="row" style={{ gap: 6 }}>
            <div
              style={{
                width: 16,
                height: 10,
                borderRadius: 2,
                background: "repeating-linear-gradient(135deg, #273137 0 4px, #182126 4px 8px)",
              }}
            />
            <span className="note">not run before cancellation</span>
          </div>
        )}
      </div>

      {/* Audit #09: this used to be a single hover-only slot that reset on
          blur. `primary` (live preview, falling back to the pinned cell) and
          `held` (a second, shift-clicked cell) can now sit side by side. */}
      <div className="row" style={{ marginTop: 10, gap: 2, alignItems: "stretch" }}>
        <div className="tooltip" style={{ flex: 1, minHeight: 68 }}>
          {primary ? (
            <RunDetail row={primary} yLabel={yLabel} xLabel={xLabel} kicker={pinned && primary.index === pinned.index ? "pinned" : undefined} />
          ) : (
            <div className="t">Click or focus a cell for its run. Shift-click a second to hold it alongside.</div>
          )}
        </div>
        {held && (
          <div className="tooltip" style={{ flex: 1, minHeight: 68 }}>
            <RunDetail row={held} yLabel={yLabel} xLabel={xLabel} kicker="held" />
          </div>
        )}
      </div>
    </div>
  );
}

function RunDetail({
  row, yLabel, xLabel, kicker,
}: {
  row: SweepRunRow;
  yLabel: string;
  xLabel: string;
  kicker?: string;
}) {
  return (
    <>
      <div className="t">
        {yLabel} = {row.coordinates[0]} · {xLabel} = {row.coordinates[1]}
        {kicker && <span style={{ marginLeft: 8, color: "var(--accent)", textTransform: "uppercase", fontSize: 10, letterSpacing: "0.08em" }}>{kicker}</span>}
      </div>
      <div>
        {row.shot_time_s.toFixed(2)} s · {row.beverage_mass_g.toFixed(1)} g ·{" "}
        {row.tds_percent.toFixed(2)} % TDS ·{" "}
        {row.extraction_yield_percent.toFixed(2)} % yield
      </div>
      <div style={{ color: "var(--muted)" }}>
        {row.termination.replace(/_/g, " ")}
        {row.warning_count > 0 && ` · ${row.warning_count} warning(s)`}
      </div>
    </>
  );
}
