import { useEffect, useMemo, useRef, useState } from "react";

import type { RegionSummary, ShotResult, ShotSample } from "../../api/types";

interface Props {
  result: ShotResult;
  targetBeverageG: number | null;
  // When the pointer is over a chart in the stack, the cross-section follows
  // that cursor instead of its own playhead: one time cursor for the whole
  // shot view (12.5), not a second, competing one.
  cursorTimeSeconds?: number;
}

const VIEW = { width: 420, height: 292 };

// One cross-section through the basket: group head at the top, puck in the
// middle, cup underneath. All geometry is fixed; only the fills move.
const BASKET = { x0: 132, x1: 288, top: 36, bottom: 172, wall: 7 };
const PUCK = { top: 96, bottom: 170 };
const SPOUT = { x: 210, tip: 194, mouthHalfWidth: 18, y: 188 };
const CUP = { rim: 206, floor: 266, rimHalfWidth: 52, floorHalfWidth: 38 };

const PUCK_WIDTH = BASKET.x1 - BASKET.x0;
const PUCK_HEIGHT = PUCK.bottom - PUCK.top;

// Colours are read at the boundary rather than through CSS variables because
// they are interpolated per frame; these mirror the palette in styles.css.
const DRY_GROUNDS = "#9a6f4a";
const WET_GROUNDS = "#4e3120";
const SPENT_GROUNDS = "#7d6250";
const COOL_WATER = "#a8d4e2";
const HOT_WATER = "#f4c088";
const WEAK_BEVERAGE = "#c9924f";
const STRONG_BEVERAGE = "#4a2410";
const CREMA = "#d3a05e";

const INLET_RANGE = [85, 100] as const;
// The puck starts near ambient and is warmed by the water, so its gauge cannot
// share the inlet's range without sitting pinned at empty for the first
// several seconds.
const PUCK_TEMPERATURE_RANGE = [20, 100] as const;
const PRESSURE_RANGE = [0, 12] as const;
const TDS_RANGE = [0, 12] as const;
const EXTRACTION_FULL = 25;

function channels(hex: string): [number, number, number] {
  return [
    parseInt(hex.slice(1, 3), 16),
    parseInt(hex.slice(3, 5), 16),
    parseInt(hex.slice(5, 7), 16),
  ];
}

function mix(from: string, to: string, amount: number): string {
  const t = Math.min(1, Math.max(0, amount));
  const a = channels(from);
  const b = channels(to);
  const blend = a.map((channel, index) => Math.round(channel + (b[index] - channel) * t));
  return `rgb(${blend[0]}, ${blend[1]}, ${blend[2]})`;
}

function normalise(value: number, [low, high]: readonly [number, number]): number {
  return Math.min(1, Math.max(0, (value - low) / (high - low)));
}

// Samples are evenly spaced, but a binary search costs nothing and survives a
// run recorded at a different sample interval.
function sampleIndexAt(samples: ShotSample[], time: number): number {
  let low = 0;
  let high = samples.length - 1;
  while (low < high) {
    const middle = (low + high) >> 1;
    if (samples[middle].time_s < time) low = middle + 1;
    else high = middle;
  }
  if (low > 0 && Math.abs(samples[low - 1].time_s - time) < Math.abs(samples[low].time_s - time)) {
    return low - 1;
  }
  return low;
}

const SINGLE_REGION: RegionSummary = {
  area_fraction: 1,
  permeability_multiplier: 1,
  beverage_mass_g: 0,
  flow_fraction: 1,
  tds_percent: 0,
  extraction_yield_percent: 0,
};

interface Column {
  region: RegionSummary;
  x: number;
  width: number;
  centre: number;
  grains: { x: number; y: number; r: number }[];
}

// A fixed integer hash, so the grain texture belongs to the region rather than
// to the render: scrubbing the timeline must not make the puck crawl.
function grainsFor(index: number, x: number, width: number, permeability: number): Column["grains"] {
  const grains: Column["grains"] = [];
  const density = Math.max(0.35, Math.min(1.6, 1 / permeability));
  const count = Math.round((width * PUCK_HEIGHT) / 150 * density);
  let seed = (index + 1) * 2654435761;
  const next = () => {
    seed = (seed ^ (seed << 13)) >>> 0;
    seed = (seed ^ (seed >>> 17)) >>> 0;
    seed = (seed ^ (seed << 5)) >>> 0;
    return seed / 4294967296;
  };
  for (let i = 0; i < count; i += 1) {
    grains.push({
      x: x + next() * width,
      y: PUCK.top + next() * PUCK_HEIGHT,
      r: 0.7 + next() * (1.5 / Math.max(0.5, density)),
    });
  }
  return grains;
}

type Phase = "dry" | "saturating" | "brewing" | "finished";

function phaseOf(sample: ShotSample, isLast: boolean): Phase {
  if (isLast) return "finished";
  if (sample.beverage_mass_g > 0.05) return "brewing";
  if (sample.saturation > 0.005) return "saturating";
  return "dry";
}

const PHASE_LABEL: Record<Phase, string> = {
  dry: "dry puck",
  saturating: "saturating — water in, pore space filling, nothing in the cup yet",
  brewing: "brewing — liquid is leaving the puck",
  finished: "shot complete",
};

// The sampled flow is the Darcy flow *into* the puck. What leaves the spout is
// the rise of the beverage mass, which is zero until the pore volume is full —
// the two are different numbers, and the whole point of the drawing is that the
// gap between them is the pre-infusion.
function cupRatesOf(samples: ShotSample[]): number[] {
  return samples.map((sample, index) => {
    if (index === 0) return 0;
    const previous = samples[index - 1];
    const span = sample.time_s - previous.time_s;
    if (span <= 0) return 0;
    return Math.max(0, (sample.beverage_mass_g - previous.beverage_mass_g) / span);
  });
}

export function PuckView({ result, targetBeverageG, cursorTimeSeconds }: Props) {
  const samples = result.samples;
  const duration = samples.length ? samples[samples.length - 1].time_s : 0;
  const runId = result.manifest.run_id;

  const [playhead, setPlayhead] = useState(0);
  const [playing, setPlaying] = useState(false);
  const [speed, setSpeed] = useState(1);
  const reducedMotion = useRef(
    typeof window !== "undefined" &&
      window.matchMedia?.("(prefers-reduced-motion: reduce)").matches === true,
  );

  // A fresh run rewinds and plays itself, which is the whole point of the
  // panel; a reader who has asked for less motion gets the first frame and the
  // scrubber instead.
  useEffect(() => {
    setPlayhead(0);
    setPlaying(!reducedMotion.current);
  }, [runId]);

  useEffect(() => {
    if (!playing || duration <= 0) return undefined;
    let frame = 0;
    let previous = performance.now();
    const tick = (now: number) => {
      const elapsed = (now - previous) / 1000;
      previous = now;
      setPlayhead((current) => {
        const next = current + elapsed * speed;
        if (next >= duration) {
          setPlaying(false);
          return duration;
        }
        return next;
      });
      frame = requestAnimationFrame(tick);
    };
    frame = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(frame);
  }, [playing, speed, duration]);

  // Touching the transport detaches from the chart cursor at its current value
  // rather than fighting it, and the next move of the cursor re-attaches. The
  // controls are therefore never left disabled by a cursor that stopped
  // updating without clearing.
  const [detachedAt, setDetachedAt] = useState<number>();
  const following = cursorTimeSeconds !== undefined && cursorTimeSeconds !== detachedAt;
  const time = following ? Math.min(cursorTimeSeconds, duration) : playhead;
  const takeControl = () => setDetachedAt(cursorTimeSeconds);

  const columns = useMemo<Column[]>(() => {
    const regions = result.regions?.length ? result.regions : [SINGLE_REGION];
    let cursor = BASKET.x0;
    return regions.map((region, index) => {
      const width = PUCK_WIDTH * region.area_fraction;
      const column: Column = {
        region,
        x: cursor,
        width,
        centre: cursor + width / 2,
        grains: grainsFor(index, cursor, width, region.permeability_multiplier),
      };
      cursor += width;
      return column;
    });
  }, [result.regions]);

  const cupRates = useMemo(() => cupRatesOf(samples), [samples]);
  const peakCupRate = useMemo(() => Math.max(0.05, ...cupRates), [cupRates]);

  if (!samples.length) return null;

  const index = sampleIndexAt(samples, time);
  const sample = samples[index];
  const phase = phaseOf(sample, index === samples.length - 1);

  const waterColour = mix(COOL_WATER, HOT_WATER, normalise(sample.inlet_temperature_c, INLET_RANGE));
  const puckWarmth = normalise(sample.puck_temperature_c, PUCK_TEMPERATURE_RANGE);
  const puckWater = mix(COOL_WATER, HOT_WATER, puckWarmth);
  const beverageColour = mix(WEAK_BEVERAGE, STRONG_BEVERAGE, normalise(sample.tds_percent, TDS_RANGE));
  const spent = Math.min(1, sample.extraction_yield_percent / EXTRACTION_FULL);
  const wetGrounds = mix(mix(DRY_GROUNDS, WET_GROUNDS, 1), SPENT_GROUNDS, spent * 0.55);

  // The fill height is the saturation number, drawn as a level that descends
  // from the screen side because that is where the water arrives. The model
  // carries one lumped saturation per region and no axial structure, so this is
  // a gauge inside the puck outline, not a computed wetting depth.
  const wetHeight = PUCK_HEIGHT * Math.min(1, Math.max(0, sample.saturation));
  const wetFront = PUCK.top + wetHeight;

  const inflowFraction = Math.min(1, sample.flow_ml_s / Math.max(result.peak_flow_ml_s, 0.1));
  const cupRate = cupRates[index];
  const cupFraction = Math.min(1, cupRate / peakCupRate);
  const streamWidth = cupRate > 0.005 ? 1 + 4.2 * Math.sqrt(cupFraction) : 0;
  const dripping = cupRate > 0.005 && cupRate < 0.35;

  const cupTarget = targetBeverageG ?? Math.max(result.beverage_mass_g, 1);
  const cupFill = Math.min(1, sample.beverage_mass_g / cupTarget);
  const cupDepth = (CUP.floor - CUP.rim - 6) * cupFill;
  const cupSurface = CUP.floor - 2 - cupDepth;
  const cremaDepth = Math.min(9, cupDepth * 0.22);

  // Droplets are placed from the timeline, not from a wall clock, so they stop
  // when the playhead stops and run backwards when it is dragged backwards.
  const dropPhase = (time * 2.6) % 1;

  const jetOpacity = Math.min(0.85, sample.pressure_bar / 9);

  return (
    <div className="chart-card">
      <div className="row" style={{ justifyContent: "space-between", alignItems: "baseline" }}>
        <h3 style={{ margin: 0 }}>Puck cross-section</h3>
        <span className="note">
          {following ? "following the chart cursor" : PHASE_LABEL[phase]}
        </span>
      </div>

      <div className="puck-view">
        <svg
          viewBox={`0 0 ${VIEW.width} ${VIEW.height}`}
          className="puck-svg"
          role="img"
          aria-label={
            `Puck cross-section at ${sample.time_s.toFixed(1)} seconds: ` +
            `${PHASE_LABEL[phase]}, saturation ${(sample.saturation * 100).toFixed(0)} percent, ` +
            `inflow ${sample.flow_ml_s.toFixed(2)} millilitres per second, ` +
            `${sample.beverage_mass_g.toFixed(1)} grams in the cup.`
          }
        >
          <defs>
            <clipPath id="puck-clip">
              <rect x={BASKET.x0} y={PUCK.top} width={PUCK_WIDTH} height={PUCK_HEIGHT} />
            </clipPath>
            <clipPath id="cup-clip">
              <path
                d={`M ${SPOUT.x - CUP.rimHalfWidth} ${CUP.rim}
                    L ${SPOUT.x + CUP.rimHalfWidth} ${CUP.rim}
                    L ${SPOUT.x + CUP.floorHalfWidth} ${CUP.floor}
                    L ${SPOUT.x - CUP.floorHalfWidth} ${CUP.floor} Z`}
              />
            </clipPath>
          </defs>

          {/* Group head and dispersion screen. */}
          <rect
            x={BASKET.x0 - 22} y={8} width={PUCK_WIDTH + 44} height={22} rx={4}
            fill="var(--panel-2)" stroke="var(--line)"
          />
          <line
            x1={BASKET.x0 - 6} x2={BASKET.x1 + 6} y1={31} y2={31}
            stroke="var(--line)" strokeWidth={2}
          />

          {/* Inlet water: colour is the commanded inlet temperature, opacity the
              commanded pressure. */}
          {sample.pressure_bar > 0.01 &&
            [0.12, 0.31, 0.5, 0.69, 0.88].map((position, jet) => {
              const x = BASKET.x0 + PUCK_WIDTH * position;
              return (
                <line
                  key={position}
                  x1={x} x2={x} y1={33} y2={PUCK.top - 1}
                  stroke={waterColour}
                  strokeWidth={1 + 2.4 * Math.sqrt(inflowFraction)}
                  strokeLinecap="round"
                  strokeDasharray="5 7"
                  strokeDashoffset={-((dropPhase + jet * 0.2) % 1) * 12}
                  opacity={jetOpacity}
                />
              );
            })}

          {/* Basket walls. */}
          <rect
            x={BASKET.x0 - BASKET.wall} y={BASKET.top} width={BASKET.wall}
            height={BASKET.bottom - BASKET.top} fill="var(--panel-2)" stroke="var(--line)"
          />
          <rect
            x={BASKET.x1} y={BASKET.top} width={BASKET.wall}
            height={BASKET.bottom - BASKET.top} fill="var(--panel-2)" stroke="var(--line)"
          />

          {/* The puck, one column per lateral region. */}
          <g clipPath="url(#puck-clip)">
            {columns.map((column, columnIndex) => (
              <g key={columnIndex}>
                <rect
                  x={column.x} y={PUCK.top} width={column.width} height={PUCK_HEIGHT}
                  fill={mix(DRY_GROUNDS, SPENT_GROUNDS, spent * 0.4)}
                />
                <rect
                  x={column.x} y={PUCK.top} width={column.width} height={wetHeight}
                  fill={wetGrounds}
                />
                {/* A thin sheen of the water's own colour on the wetted pores. */}
                <rect
                  x={column.x} y={PUCK.top} width={column.width} height={wetHeight}
                  fill={puckWater} opacity={0.16 * Math.min(1, sample.saturation * 1.4)}
                />
                {column.grains.map((grain, grainIndex) => (
                  <circle
                    key={grainIndex}
                    cx={grain.x} cy={grain.y} r={grain.r}
                    fill={grain.y <= wetFront ? "#1d120a" : "#6d4b30"}
                    opacity={grain.y <= wetFront ? 0.45 : 0.55}
                  />
                ))}
                {columnIndex > 0 && (
                  <line
                    x1={column.x} x2={column.x} y1={PUCK.top} y2={PUCK.bottom}
                    stroke="var(--line)" strokeWidth={0.8} strokeDasharray="2 3"
                  />
                )}
              </g>
            ))}
          </g>

          {/* Saturation level line and its readout. */}
          <line
            x1={BASKET.x0} x2={BASKET.x1} y1={wetFront} y2={wetFront}
            stroke={COOL_WATER} strokeWidth={1.2}
            opacity={sample.saturation > 0.002 && sample.saturation < 0.999 ? 0.9 : 0}
          />
          <text
            x={BASKET.x1 + BASKET.wall + 5} y={wetFront + 3} fontSize={8} fill="var(--muted)"
            style={{ fontFamily: "ui-monospace, Menlo, monospace" }}
          >
            S {sample.saturation.toFixed(2)}
          </text>
          <rect
            x={BASKET.x0} y={PUCK.top} width={PUCK_WIDTH} height={PUCK_HEIGHT}
            fill="none" stroke="var(--line)" strokeWidth={0.8}
          />

          {/* Region labels: the permeability multiplier is a recipe input, so it
              can be named on the drawing. */}
          {columns.length > 1 &&
            columns.map((column, columnIndex) => (
              <text
                key={columnIndex}
                x={column.centre} y={PUCK.top - 5} fontSize={7.5} textAnchor="middle"
                fill="var(--muted)" stroke="var(--panel)" strokeWidth={2.5}
                paintOrder="stroke"
                style={{ fontFamily: "ui-monospace, Menlo, monospace" }}
              >
                k x{column.region.permeability_multiplier}
              </text>
            ))}

          {/* Basket floor tapering into one spout. */}
          <path
            d={`M ${BASKET.x0 - BASKET.wall} ${BASKET.bottom}
                L ${BASKET.x1 + BASKET.wall} ${BASKET.bottom}
                L ${SPOUT.x + SPOUT.mouthHalfWidth} ${SPOUT.y}
                L ${SPOUT.x + SPOUT.mouthHalfWidth - 5} ${SPOUT.tip}
                L ${SPOUT.x - SPOUT.mouthHalfWidth + 5} ${SPOUT.tip}
                L ${SPOUT.x - SPOUT.mouthHalfWidth} ${SPOUT.y} Z`}
            fill="var(--panel-2)" stroke="var(--line)"
          />

          {/* Each region's share of the flow, converging on the spout. */}
          {streamWidth > 0 &&
            columns.map((column, columnIndex) => (
              <path
                key={columnIndex}
                d={`M ${column.centre} ${PUCK.bottom}
                    Q ${column.centre} ${SPOUT.y} ${SPOUT.x} ${SPOUT.tip}`}
                fill="none"
                stroke={beverageColour}
                strokeWidth={Math.max(0.6, streamWidth * Math.sqrt(column.region.flow_fraction))}
                strokeLinecap="round"
                opacity={0.85}
              />
            ))}

          {/* The stream into the cup: continuous at speed, drops when it is slow. */}
          {streamWidth > 0 && !dripping && (
            <rect
              x={SPOUT.x - streamWidth / 2} y={SPOUT.tip}
              width={streamWidth} height={Math.max(0, cupSurface - SPOUT.tip)}
              fill={beverageColour}
            />
          )}
          {dripping &&
            [0, 0.5].map((offset) => {
              const progress = (dropPhase + offset) % 1;
              const y = SPOUT.tip + progress * Math.max(0, cupSurface - SPOUT.tip);
              return (
                <ellipse
                  key={offset}
                  cx={SPOUT.x} cy={y} rx={2} ry={3.2}
                  fill={beverageColour}
                />
              );
            })}

          {/* Cup. */}
          <g clipPath="url(#cup-clip)">
            <rect
              x={SPOUT.x - CUP.rimHalfWidth} y={cupSurface}
              width={CUP.rimHalfWidth * 2} height={CUP.floor - cupSurface}
              fill={beverageColour}
            />
            {cremaDepth > 0.5 && (
              <rect
                x={SPOUT.x - CUP.rimHalfWidth} y={cupSurface}
                width={CUP.rimHalfWidth * 2} height={cremaDepth}
                fill={CREMA} opacity={0.9}
              />
            )}
          </g>
          <path
            d={`M ${SPOUT.x - CUP.rimHalfWidth} ${CUP.rim}
                L ${SPOUT.x + CUP.rimHalfWidth} ${CUP.rim}
                L ${SPOUT.x + CUP.floorHalfWidth} ${CUP.floor}
                L ${SPOUT.x - CUP.floorHalfWidth} ${CUP.floor} Z`}
            fill="none" stroke="var(--line)" strokeWidth={1.4}
          />
          <path
            d={`M ${SPOUT.x + CUP.rimHalfWidth} ${CUP.rim + 8}
                q 18 4 14 18 q -4 14 -20 16`}
            fill="none" stroke="var(--line)" strokeWidth={1.4}
          />
          {cupFill > 0 && (
            <text
              x={SPOUT.x + CUP.rimHalfWidth + 26} y={cupSurface + 3} fontSize={8}
              fill="var(--muted)" style={{ fontFamily: "ui-monospace, Menlo, monospace" }}
            >
              {sample.beverage_mass_g.toFixed(1)} g
            </text>
          )}

          {/* Left-hand gutter: pressure gauge and puck temperature, each with its
              own scale printed so neither bar is a bare fraction. */}
          {(
            [
              {
                x: 16,
                title: "bar",
                fraction: normalise(sample.pressure_bar, PRESSURE_RANGE),
                readout: sample.pressure_bar.toFixed(1),
                top: PRESSURE_RANGE[1],
                bottom: PRESSURE_RANGE[0],
                fill: "var(--accent)",
              },
              {
                x: 62,
                title: "puck °C",
                fraction: puckWarmth,
                readout: sample.puck_temperature_c.toFixed(1),
                top: PUCK_TEMPERATURE_RANGE[1],
                bottom: PUCK_TEMPERATURE_RANGE[0],
                fill: puckWater,
              },
            ] as const
          ).map((gauge) => {
            const top = PUCK.top - 16;
            const height = PUCK_HEIGHT + 30;
            return (
              <g key={gauge.title}>
                <text x={gauge.x} y={top - 14} fontSize={7.5} fill="var(--muted)">
                  {gauge.title}
                </text>
                <text x={gauge.x + 13} y={top + 4} fontSize={6.5} fill="var(--muted)">
                  {gauge.top}
                </text>
                <text x={gauge.x + 13} y={top + height} fontSize={6.5} fill="var(--muted)">
                  {gauge.bottom}
                </text>
                <rect
                  x={gauge.x} y={top} width={10} height={height} rx={2}
                  fill="var(--bg)" stroke="var(--line)" strokeWidth={0.8}
                />
                <rect
                  x={gauge.x + 1} y={top + 1 + (height - 2) * (1 - gauge.fraction)}
                  width={8} height={(height - 2) * gauge.fraction}
                  fill={gauge.fill} opacity={0.85}
                />
                <text
                  x={gauge.x} y={top + height + 12} fontSize={8} fill="var(--muted)"
                  style={{ fontFamily: "ui-monospace, Menlo, monospace" }}
                >
                  {gauge.readout}
                </text>
              </g>
            );
          })}

        </svg>

        <div className="puck-readout">
          <div className="kv">
            <span className="k">t</span>
            <span>{sample.time_s.toFixed(2)} s</span>
            <span className="k">saturation</span>
            <span>{(sample.saturation * 100).toFixed(1)} %</span>
            <span className="k">flow in</span>
            <span>{sample.flow_ml_s.toFixed(2)} ml/s</span>
            <span className="k">to cup</span>
            <span>{cupRate.toFixed(2)} g/s</span>
            <span className="k">in cup</span>
            <span>{sample.beverage_mass_g.toFixed(2)} g</span>
            <span className="k">TDS</span>
            <span>{sample.tds_percent.toFixed(2)} %</span>
            <span className="k">extraction</span>
            <span>{sample.extraction_yield_percent.toFixed(2)} %</span>
            <span className="k">inlet</span>
            <span>{sample.inlet_temperature_c.toFixed(1)} °C</span>
          </div>
          <p className="note" style={{ marginTop: 10 }}>
            Every value on the drawing is the solver sample nearest the playhead — the panel
            interpolates nothing. The jets above the puck are the sampled Darcy flow{" "}
            <em>into</em> the bed; the spout carries the rise of the beverage mass between
            neighbouring samples, which is why it stays dry while the pores fill. Fill height inside
            the puck is the reported saturation drawn as a level — the model lumps saturation per
            region and resolves no axial structure, so it is a gauge, not a wetting depth. Stream
            thickness splits by each region's share of the run's total flow, which the solver
            reports once per run rather than per sample.
          </p>
        </div>
      </div>

      <div className="puck-transport">
        <button
          className="ghost"
          onClick={() => {
            takeControl();
            if (playhead >= duration) setPlayhead(0);
            setPlaying((current) => !current);
          }}
          aria-label={playing ? "Pause playback" : "Play the shot"}
        >
          {playing ? "Pause" : playhead >= duration ? "Replay" : "Play"}
        </button>
        <input
          type="range"
          min={0}
          max={duration}
          step={result.manifest.sample_interval_s || 0.05}
          value={time}
          aria-label="Shot time"
          onChange={(event) => {
            takeControl();
            setPlaying(false);
            setPlayhead(Number(event.target.value));
          }}
        />
        <span className="puck-clock">
          {time.toFixed(1)} / {duration.toFixed(1)} s
        </span>
        <select
          value={speed}
          aria-label="Playback speed"
          style={{ width: 70 }}
          onChange={(event) => setSpeed(Number(event.target.value))}
        >
          <option value={0.25}>0.25x</option>
          <option value={0.5}>0.5x</option>
          <option value={1}>1x</option>
          <option value={2}>2x</option>
          <option value={4}>4x</option>
        </select>
      </div>
    </div>
  );
}
