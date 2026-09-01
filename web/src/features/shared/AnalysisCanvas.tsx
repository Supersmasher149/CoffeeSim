import { useMemo, type MouseEvent } from "react";

import { CursorReadout, type CursorReadoutRow } from "./CursorReadout";

export interface CanvasPoint {
  time: number;
  value: number;
}

export interface CanvasSeries {
  key: string;
  label: string;
  color: string;
  width?: number;
  dash?: string;
  /** Overlay/comparison traces render lighter so the primary run stays legible. */
  opacity?: number;
  format?: (value: number) => string;
  points: CanvasPoint[];
}

export interface CanvasMarker {
  time: number;
  label: string;
  color: string;
}

interface Props {
  /** Accessible summary for the well -- recharts spoke for itself via its own
   * ARIA plumbing; a hand-rolled SVG has none, so this is load-bearing. */
  ariaLabel: string;
  series: CanvasSeries[];
  residual?: CanvasSeries;
  residualLabel?: string;
  markers?: CanvasMarker[];
  preInfusionEnd?: number;
  cursorTimeSeconds?: number;
  onCursorChange?: (time: number | undefined) => void;
  height?: number;
}

// Audit #03/#04: this single well replaces per-signal chart-selector charts
// (Shot) and separate mass/residual LineCharts (Calibration). Every series
// shares one time axis and one cursor; each is normalized to its own value
// range within the plot band since pressure/flow/mass/temperature have no
// common unit -- exact values live in the CursorReadout and the data table,
// not on a literal shared y-axis.
const WIDTH = 1000;
const MAIN_HEIGHT = 330;
const RESIDUAL_HEIGHT = 90;
const PAD = 10;

function timeDomain(allSeries: CanvasSeries[]): [number, number] {
  let min = Infinity;
  let max = -Infinity;
  for (const line of allSeries) {
    for (const point of line.points) {
      if (point.time < min) min = point.time;
      if (point.time > max) max = point.time;
    }
  }
  if (!Number.isFinite(min) || !Number.isFinite(max) || min >= max) {
    return [0, Number.isFinite(max) && max > 0 ? max : 1];
  }
  return [min, max];
}

function valueDomain(points: CanvasPoint[]): [number, number] {
  if (points.length === 0) return [0, 1];
  let min = points[0].value;
  let max = points[0].value;
  for (const point of points) {
    if (point.value < min) min = point.value;
    if (point.value > max) max = point.value;
  }
  if (min === max) {
    min -= 1;
    max += 1;
  }
  return [min, max];
}

function buildPath(
  points: CanvasPoint[],
  tMin: number,
  tMax: number,
  vMin: number,
  vMax: number,
  top: number,
  bottom: number,
): string {
  if (points.length === 0) return "";
  const sorted = [...points].sort((a, b) => a.time - b.time);
  const x = (t: number) => (tMax === tMin ? 0 : ((t - tMin) / (tMax - tMin)) * WIDTH);
  const y = (v: number) =>
    vMax === vMin ? (top + bottom) / 2 : bottom - ((v - vMin) / (vMax - vMin)) * (bottom - top);
  return sorted.map((point, index) => `${index === 0 ? "M" : "L"}${x(point.time).toFixed(2)},${y(point.value).toFixed(2)}`).join(" ");
}

function nearestPoint(points: CanvasPoint[], time: number): CanvasPoint | undefined {
  let best: CanvasPoint | undefined;
  let bestDelta = Infinity;
  for (const point of points) {
    const delta = Math.abs(point.time - time);
    if (delta < bestDelta) {
      bestDelta = delta;
      best = point;
    }
  }
  return best;
}

export function AnalysisCanvas({
  ariaLabel,
  series,
  residual,
  residualLabel,
  markers = [],
  preInfusionEnd,
  cursorTimeSeconds,
  onCursorChange,
  height,
}: Props) {
  const allSeries = residual ? [...series, residual] : series;
  const [tMin, tMax] = useMemo(() => timeDomain(allSeries), [allSeries]);
  const totalHeight = MAIN_HEIGHT + (residual ? RESIDUAL_HEIGHT : 0);

  const handleMove = (event: MouseEvent<SVGSVGElement>) => {
    if (!onCursorChange) return;
    const rect = event.currentTarget.getBoundingClientRect();
    if (rect.width === 0) return;
    const ratio = Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width));
    onCursorChange(tMin + ratio * (tMax - tMin));
  };

  const cursorX =
    cursorTimeSeconds !== undefined && tMax > tMin
      ? ((cursorTimeSeconds - tMin) / (tMax - tMin)) * WIDTH
      : undefined;
  const preInfusionX =
    preInfusionEnd !== undefined && tMax > tMin
      ? ((preInfusionEnd - tMin) / (tMax - tMin)) * WIDTH
      : undefined;

  const readoutRows: CursorReadoutRow[] =
    cursorTimeSeconds === undefined
      ? []
      : allSeries.map((line) => {
          const point = nearestPoint(line.points, cursorTimeSeconds);
          return {
            key: line.key,
            label: line.label,
            color: line.color,
            formatted: point ? (line.format ? line.format(point.value) : point.value.toFixed(2)) : "—",
          };
        });

  return (
    <div className="chart-card analysis-canvas" role="img" aria-label={ariaLabel}>
      <div className="analysis-canvas-well" style={{ height: height ?? 360 }}>
        <svg
          viewBox={`0 0 ${WIDTH} ${totalHeight}`}
          preserveAspectRatio="none"
          className="analysis-canvas-svg"
          onMouseMove={handleMove}
          onMouseLeave={() => onCursorChange?.(undefined)}
        >
          {[0.2, 0.4, 0.6, 0.8].map((fraction) => (
            <line
              key={`h-${fraction}`}
              x1={0} y1={MAIN_HEIGHT * fraction} x2={WIDTH} y2={MAIN_HEIGHT * fraction}
              className="analysis-gridline"
            />
          ))}
          {[0.25, 0.5, 0.75].map((fraction) => (
            <line
              key={`v-${fraction}`}
              x1={WIDTH * fraction} y1={0} x2={WIDTH * fraction} y2={MAIN_HEIGHT}
              className="analysis-gridline"
            />
          ))}

          {preInfusionX !== undefined && (
            <>
              <rect x={0} y={0} width={preInfusionX} height={MAIN_HEIGHT} className="analysis-preinfusion" />
              <line
                x1={preInfusionX} y1={0} x2={preInfusionX} y2={MAIN_HEIGHT}
                className="analysis-preinfusion-divider"
              />
            </>
          )}

          {markers.map((marker) => {
            const x = tMax > tMin ? ((marker.time - tMin) / (tMax - tMin)) * WIDTH : 0;
            return (
              <line
                key={`marker-${marker.label}-${marker.time}`}
                x1={x} y1={0} x2={x} y2={MAIN_HEIGHT}
                stroke={marker.color} strokeDasharray="3 3" strokeWidth={1} opacity={0.65}
              />
            );
          })}

          {series.map((line) => {
            const [vMin, vMax] = valueDomain(line.points);
            return (
              <path
                key={line.key}
                data-series={line.key}
                d={buildPath(line.points, tMin, tMax, vMin, vMax, PAD, MAIN_HEIGHT - PAD)}
                fill="none"
                stroke={line.color}
                strokeWidth={line.width ?? 2}
                strokeDasharray={line.dash}
                opacity={line.opacity ?? 1}
              />
            );
          })}

          {residual && (
            <>
              <line
                x1={0} y1={MAIN_HEIGHT + RESIDUAL_HEIGHT / 2} x2={WIDTH} y2={MAIN_HEIGHT + RESIDUAL_HEIGHT / 2}
                className="analysis-zero-rule" strokeDasharray="3 4"
              />
              {(() => {
                const [rMin, rMax] = valueDomain(residual.points);
                const bound = Math.max(Math.abs(rMin), Math.abs(rMax)) || 1;
                return (
                  <path
                    data-series={residual.key}
                    d={buildPath(
                      residual.points, tMin, tMax, -bound, bound,
                      MAIN_HEIGHT + 6, MAIN_HEIGHT + RESIDUAL_HEIGHT - 6,
                    )}
                    fill="none"
                    stroke={residual.color}
                    strokeWidth={residual.width ?? 1.5}
                  />
                );
              })()}
            </>
          )}

          {cursorX !== undefined && (
            <line x1={cursorX} y1={0} x2={cursorX} y2={totalHeight} className="analysis-cursor" />
          )}
        </svg>

        {residual && residualLabel && (
          <div className="analysis-residual-label" style={{ top: `${(MAIN_HEIGHT / totalHeight) * 100}%` }}>
            {residualLabel}
          </div>
        )}

        {cursorTimeSeconds !== undefined && cursorX !== undefined && (
          <CursorReadout
            timeSeconds={cursorTimeSeconds}
            xPercent={(cursorX / WIDTH) * 100}
            rows={readoutRows}
          />
        )}
      </div>
    </div>
  );
}
