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
              strokeDasharray={line.dashed ? "4 3" : undefined}
            />
          ))}
        </LineChart>
      </ResponsiveContainer>
    </div>
  );
}

export function ChartStack({ result, comparisons, preInfusionEnd, onCursorChange }: Props) {
  // Overlay runs are merged onto one time axis by sample index; the solver uses
  // the same sample interval for every run, so the axes line up.
  const runs = [result, ...comparisons].slice(0, 3);
  const data = result.samples.map((sample, index) => {
    const row: Record<string, number> = { ...sample };
    runs.forEach((run, runIndex) => {
      if (runIndex === 0) return;
      const other = run.samples[index];
      if (!other) return;
      row[`pressure_bar_${runIndex}`] = other.pressure_bar;
      row[`flow_ml_s_${runIndex}`] = other.flow_ml_s;
      row[`beverage_mass_g_${runIndex}`] = other.beverage_mass_g;
      row[`extraction_yield_percent_${runIndex}`] = other.extraction_yield_percent;
      row[`puck_temperature_c_${runIndex}`] = other.puck_temperature_c;
      row[`tds_percent_${runIndex}`] = other.tds_percent;
    });
    return row;
  });

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

  return (
    <>
      {/* Commanded pressure is shown separately from computed flow (12.5). */}
      <Chart
        title="Commanded pressure (bar)" unit="bar" data={data} markers={markers}
        onCursorChange={onCursorChange}
        series={[{ key: "pressure_bar", label: "commanded pressure", color: palette[0] },
                 ...overlay("pressure_bar", "pressure")]}
      />
      <Chart
        title="Computed flow (ml/s)" unit="ml/s" data={data} markers={markers}
        onCursorChange={onCursorChange}
        series={[{ key: "flow_ml_s", label: "flow", color: palette[0] },
                 ...overlay("flow_ml_s", "flow")]}
      />
      <Chart
        title="Temperatures (°C)" unit="°C" data={data} markers={markers}
        onCursorChange={onCursorChange}
        series={[
          { key: "inlet_temperature_c", label: "inlet", color: palette[1], dashed: true },
          { key: "puck_temperature_c", label: "puck", color: palette[0] },
          ...overlay("puck_temperature_c", "puck"),
        ]}
      />
      <Chart
        title="Beverage mass (g)" unit="g" data={data} markers={markers}
        onCursorChange={onCursorChange}
        series={[{ key: "beverage_mass_g", label: "beverage mass", color: palette[0] },
                 ...overlay("beverage_mass_g", "mass")]}
      />
      <Chart
        title="Strength and extraction (%)" unit="%" data={data} markers={markers}
        onCursorChange={onCursorChange}
        series={[
          { key: "tds_percent", label: "TDS", color: palette[1] },
          { key: "extraction_yield_percent", label: "extraction yield", color: palette[0] },
          ...overlay("extraction_yield_percent", "yield"),
        ]}
      />
    </>
  );
}
