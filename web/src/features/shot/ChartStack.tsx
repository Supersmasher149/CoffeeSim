import { useId, useState } from "react";
import {
  CartesianGrid,
  Line,
  LineChart,
  ReferenceLine,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from "recharts";

import type { ShotResult } from "../../api/types";

interface Series {
  key: string;
  label: string;
  color: string;
  dashed?: boolean;
}

interface Props {
  result: ShotResult;
  comparisons: ShotResult[];
  preInfusionEnd?: number;
  onCursorChange: (time?: number) => void;
}

const palette = ["#d98b4a", "#6fb3c8", "#9c8ad0", "#78b06a"];

// Recharts' syncId gives every chart in the stack one shared hover cursor and
// one shared time axis, which is the requirement in 12.5 and the acceptance
// criterion for FR-06.
const SYNC_ID = "espressolab-shot";

type ChartKey = "pressure" | "flow" | "temperature" | "mass" | "strength";

function ShotTooltip({ active, payload, label }: any) {
  if (!active || !payload?.length) return null;
  return (
    <div className="tooltip">
      <div className="t">t = {Number(label).toFixed(2)} s</div>
      {payload.map((entry: any) => (
        <div key={entry.name} style={{ color: entry.color }}>
          {entry.name}: {Number(entry.value).toFixed(2)}
        </div>
      ))}
    </div>
  );
}

function Chart({
  title,
  data,
  series,
  unit,
  markers,
  onCursorChange,
}: {
  title: string;
  data: Record<string, number>[];
  series: Series[];
  unit: string;
  markers: { time: number; label: string; color: string }[];
  onCursorChange: (time?: number) => void;
}) {
  // Recharts renders an SVG with no text alternative of its own, so a screen
  // reader sees the chart card's heading and then nothing about what the
  // lines actually show. A one-sentence summary -- the series names and
  // however many timeline markers this chart carries -- fills that gap
  // without trying to narrate every data point.
  const summary =
    `${title}: ${series.map((line) => line.label).join(", ")} over the shot's timeline` +
    (markers.length > 0 ? `, with ${markers.length} marked event${markers.length === 1 ? "" : "s"}.` : ".");

  return (
    <div className="chart-card" role="img" aria-label={summary}>
      <h3>{title}</h3>
      <ResponsiveContainer width="100%" height={170}>
        <LineChart
          data={data}
          syncId={SYNC_ID}
          margin={{ top: 4, right: 12, bottom: 4, left: 4 }}
          onMouseMove={(state: any) => onCursorChange(state?.activeLabel)}
          onMouseLeave={() => onCursorChange(undefined)}
        >
          <CartesianGrid stroke="#3a302a" strokeDasharray="2 4" />
          <XAxis
            dataKey="time_s" type="number" domain={["dataMin", "dataMax"]}
            tick={{ fill: "#a2938a", fontSize: 11 }} stroke="#3a302a"
            label={{ value: "time (s)", position: "insideBottom", offset: -2, fill: "#a2938a", fontSize: 11 }}
          />
          <YAxis
            tick={{ fill: "#a2938a", fontSize: 11 }} stroke="#3a302a" width={64}
            label={{ value: unit, angle: -90, position: "insideLeft", offset: 12,
                     fill: "#a2938a", fontSize: 11 }}
          />
          <Tooltip content={<ShotTooltip />} />
          {markers.map((marker) => (
            <ReferenceLine
              key={marker.label + marker.time}
              x={marker.time}
              stroke={marker.color}
              strokeDasharray="3 3"
              label={{ value: marker.label, fill: marker.color, fontSize: 10, position: "top" }}
            />
          ))}
          {series.map((line) => (
            <Line
              key={line.key} type="monotone" dataKey={line.key} name={line.label}
              stroke={line.color} strokeWidth={1.8} dot={false} isAnimationActive={false}
              connectNulls
              strokeDasharray={line.dashed ? "4 3" : undefined}
            />
          ))}
        </LineChart>
      </ResponsiveContainer>
    </div>
  );
}

export function ChartStack({ result, comparisons, preInfusionEnd, onCursorChange }: Props) {
  const [selectedChart, setSelectedChart] = useState<ChartKey>("pressure");
  const selectorId = useId();
  const runs = [result, ...comparisons].slice(0, 3);
  const rowsByTime = new Map<number, Record<string, number>>();
  for (const sample of result.samples) rowsByTime.set(sample.time_s, { ...sample });
  runs.slice(1).forEach((run, comparisonIndex) => {
    const runIndex = comparisonIndex + 1;
    for (const sample of run.samples) {
      const row = rowsByTime.get(sample.time_s) ?? { time_s: sample.time_s };
      row[`pressure_bar_${runIndex}`] = sample.pressure_bar;
      row[`flow_ml_s_${runIndex}`] = sample.flow_ml_s;
      row[`beverage_mass_g_${runIndex}`] = sample.beverage_mass_g;
      row[`extraction_yield_percent_${runIndex}`] = sample.extraction_yield_percent;
      row[`puck_temperature_c_${runIndex}`] = sample.puck_temperature_c;
      row[`tds_percent_${runIndex}`] = sample.tds_percent;
      rowsByTime.set(sample.time_s, row);
    }
  });
  const data = [...rowsByTime.values()].sort((left, right) => left.time_s - right.time_s);

  const overlay = (key: string, label: string): Series[] =>
    runs.slice(1).map((run, index) => ({
      key: `${key}_${index + 1}`,
      label: `${label} · ${run.manifest.run_id.slice(5, 11)}`,
      color: palette[index + 1],
      dashed: true,
    }));

  const markers = [
    ...(preInfusionEnd !== undefined
      ? [{ time: preInfusionEnd, label: "pre-infusion end", color: "#a2938a" }]
      : []),
    ...(result.target_mass_reached
      ? [{ time: result.elapsed_time_s, label: "target mass", color: "#78b06a" }]
      : []),
    // Every warning gets a mark on the timeline: clamps are never silent (FR-08).
    ...result.warnings.map((warning) => ({
      time: warning.time_s,
      label: warning.code.toLowerCase().replace(/_/g, " "),
      color: warning.severity === "hard" ? "#d9584a" : "#e0a03a",
    })),
  ];

  const charts: Record<ChartKey, { title: string; unit: string; series: Series[] }> = {
    pressure: {
      title: "Commanded pressure (bar)",
      unit: "bar",
      series: [
        { key: "pressure_bar", label: "commanded pressure", color: palette[0] },
        ...overlay("pressure_bar", "pressure"),
      ],
    },
    flow: {
      title: "Computed flow (ml/s)",
      unit: "ml/s",
      series: [
        { key: "flow_ml_s", label: "flow", color: palette[0] },
        ...overlay("flow_ml_s", "flow"),
      ],
    },
    temperature: {
      title: "Temperatures (°C)",
      unit: "°C",
      series: [
        { key: "inlet_temperature_c", label: "inlet", color: palette[1], dashed: true },
        { key: "puck_temperature_c", label: "puck", color: palette[0] },
        ...overlay("puck_temperature_c", "puck"),
      ],
    },
    mass: {
      title: "Beverage mass (g)",
      unit: "g",
      series: [
        { key: "beverage_mass_g", label: "beverage mass", color: palette[0] },
        ...overlay("beverage_mass_g", "mass"),
      ],
    },
    strength: {
      title: "Strength and extraction (%)",
      unit: "%",
      series: [
        { key: "tds_percent", label: "TDS", color: palette[1] },
        { key: "extraction_yield_percent", label: "extraction yield", color: palette[0] },
        ...overlay("extraction_yield_percent", "yield"),
      ],
    },
  };
  const selected = charts[selectedChart];

  return (
    <section className="analysis-surface" aria-labelledby="shot-analysis-title">
      <div className="analysis-heading">
        <div>
          <p className="eyebrow">Synchronized timeline</p>
          <h2 id="shot-analysis-title">Shot analysis</h2>
        </div>
        <label htmlFor={selectorId}>
          Signal
          <select
            id={selectorId}
            value={selectedChart}
            onChange={(event) => setSelectedChart(event.target.value as ChartKey)}
          >
            <option value="pressure">Commanded pressure</option>
            <option value="flow">Computed flow</option>
            <option value="temperature">Temperatures</option>
            <option value="mass">Beverage mass</option>
            <option value="strength">Strength and extraction</option>
          </select>
        </label>
      </div>

      <div className="event-lane" aria-label="Shot timeline events">
        {markers.length > 0 ? markers.map((marker, index) => (
          <span key={`${marker.label}-${marker.time}-${index}`} style={{ borderColor: marker.color }}>
            <b>{marker.time.toFixed(1)} s</b> {marker.label}
          </span>
        )) : <span>No warnings or stop events were recorded.</span>}
      </div>

      <Chart
        title={selected.title} unit={selected.unit} data={data} markers={markers}
        onCursorChange={onCursorChange}
        series={selected.series}
      />

      <details className="analysis-data drawer">
        <summary>View current-run data table ({result.samples.length} samples)</summary>
        <div className="table-scroll">
          <table>
            <thead>
              <tr><th>time (s)</th><th>pressure (bar)</th><th>flow (ml/s)</th><th>mass (g)</th><th>TDS (%)</th><th>yield (%)</th></tr>
            </thead>
            <tbody>
              {result.samples.map((sample) => (
                <tr key={sample.time_s}>
                  <td>{sample.time_s.toFixed(2)}</td>
                  <td>{sample.pressure_bar.toFixed(2)}</td>
                  <td>{sample.flow_ml_s.toFixed(2)}</td>
                  <td>{sample.beverage_mass_g.toFixed(2)}</td>
                  <td>{sample.tds_percent.toFixed(2)}</td>
                  <td>{sample.extraction_yield_percent.toFixed(2)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </details>
    </section>
  );
}
