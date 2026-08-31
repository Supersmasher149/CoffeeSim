import { Fragment, useState } from "react";

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
export function HeatMap({
  rows, xValues, yValues, xLabel, yLabel, metric, metricLabel, metricUnit, partial = false,
}: Props) {
  const [hovered, setHovered] = useState<SweepRunRow>();

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
        "Tab through cells for each run's detail."
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
                {xValues.map((x) => {
                  const row = byCoordinate.get(`${y}|${x}`);
                  const invalid = !row || row.termination === "invalid_state";
                  const t = row ? (row[metric] - min) / span : 0;
                  // A hover-only tooltip leaves color as the only carrier of
                  // the value and is unreachable from the keyboard. Each cell
                  // is a real button: focus/blur mirror the hover handlers so
                  // Tab reaches the same detail panel a mouse does, and the
                  // aria-label is the non-color alternative to the ramp.
                  const label = !row
                    ? `${yLabel} ${formatTick(y)}, ${xLabel} ${formatTick(x)}: ` +
                      (partial ? "not run before cancellation" : "no run")
                    : invalid
                      ? `${yLabel} ${formatTick(y)}, ${xLabel} ${formatTick(x)}: outside the supported input range`
                      : `${yLabel} ${formatTick(y)}, ${xLabel} ${formatTick(x)}: ` +
                        `${metricLabel} ${format(row[metric])} ${metricUnit}, ${row.termination.replace(/_/g, " ")}`;
                  return (
                    <button
                      key={`${y}-${x}`}
                      type="button"
                      disabled={!row}
                      aria-label={label}
                      onMouseEnter={() => setHovered(row)}
                      onMouseLeave={() => setHovered(undefined)}
                      onFocus={() => setHovered(row)}
                      onBlur={() => setHovered(undefined)}
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
                          hovered && row && hovered.index === row.index
                            ? "2px solid var(--text)"
                            : "none",
                        outlineOffset: -2,
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

      {/* Per-cell hover rather than a number in every cell: mid-ramp steps
          cannot carry legible text against either ink. */}
      <div className="tooltip" style={{ marginTop: 10, minHeight: 68 }}>
        {hovered ? (
          <>
            <div className="t">
              {yLabel} = {hovered.coordinates[0]} · {xLabel} = {hovered.coordinates[1]}
            </div>
            <div>
              {hovered.shot_time_s.toFixed(2)} s · {hovered.beverage_mass_g.toFixed(1)} g ·{" "}
              {hovered.tds_percent.toFixed(2)} % TDS ·{" "}
              {hovered.extraction_yield_percent.toFixed(2)} % yield
            </div>
            <div style={{ color: "var(--muted)" }}>
              {hovered.termination.replace(/_/g, " ")}
              {hovered.warning_count > 0 && ` · ${hovered.warning_count} warning(s)`}
            </div>
          </>
        ) : (
          <div className="t">Hover or focus a cell for its run.</div>
        )}
      </div>
    </div>
  );
}
