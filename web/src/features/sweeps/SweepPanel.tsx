import { useState } from "react";
import {
  CartesianGrid,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from "recharts";

import { api } from "../../api/client";
import type { Recipe, SweepResult } from "../../api/types";
import { HeatMap, type HeatMetric } from "./HeatMap";

interface Props {
  baseline: Recipe;
  parameters: string[];
  onError: (message: string) => void;
}

interface AxisDraft {
  parameterPath: string;
  from: number;
  to: number;
  steps: number;
}

function linspace({ from, to, steps }: AxisDraft): number[] {
  if (steps < 2) return [from];
  return Array.from({ length: steps }, (_, i) =>
    Number((from + ((to - from) * i) / (steps - 1)).toFixed(6)),
  );
}

const METRICS: { key: HeatMetric; label: string; unit: string }[] = [
  { key: "shot_time_s", label: "shot time", unit: "s" },
  { key: "extraction_yield_percent", label: "extraction yield", unit: "%" },
  { key: "tds_percent", label: "TDS", unit: "%" },
  { key: "beverage_mass_g", label: "beverage mass", unit: "g" },
];

function AxisControls({
  axis, onChange, parameters, label,
}: {
  axis: AxisDraft;
  onChange: (axis: AxisDraft) => void;
  parameters: string[];
  label: string;
}) {
  return (
    <div className="row" style={{ flexWrap: "wrap", gap: 8 }}>
      <span className="note" style={{ width: 54 }}>{label}</span>
      <select
        style={{ width: 220 }}
        value={axis.parameterPath}
        onChange={(e) => onChange({ ...axis, parameterPath: e.target.value })}
      >
        {parameters.map((path) => (
          <option key={path} value={path}>{path}</option>
        ))}
      </select>
      <div className="field" style={{ margin: 0 }}>
        <label>from</label>
        <input type="number" value={axis.from}
               onChange={(e) => onChange({ ...axis, from: Number(e.target.value) })} />
      </div>
      <div className="field" style={{ margin: 0 }}>
        <label>to</label>
        <input type="number" value={axis.to}
               onChange={(e) => onChange({ ...axis, to: Number(e.target.value) })} />
      </div>
      <div className="field" style={{ margin: 0 }}>
        <label>steps</label>
        <input type="number" min={2} max={40} value={axis.steps}
               onChange={(e) => onChange({ ...axis, steps: Number(e.target.value) })} />
      </div>
    </div>
  );
}

// Section 12.3, experiment view. One axis plots a metric line; two axes plot a
// heat map (section 11.1). Sweeps run synchronously in this build (15.2), so
// "progress" is a busy state.
export function SweepPanel({ baseline, parameters, onError }: Props) {
  const [primary, setPrimary] = useState<AxisDraft>({
    parameterPath: "puck.particle_diameter_um", from: 250, to: 450, steps: 9,
  });
  const [secondary, setSecondary] = useState<AxisDraft>({
    parameterPath: "temperature_profile_c.constant", from: 88, to: 96, steps: 5,
  });
  const [twoDimensional, setTwoDimensional] = useState(false);
  const [metric, setMetric] = useState<HeatMetric>("shot_time_s");
  const [result, setResult] = useState<SweepResult>();
  const [running, setRunning] = useState(false);

  // The second axis is the outer one, so the primary axis varies fastest and
  // stays on the horizontal axis of the heat map.
  const axes = twoDimensional ? [secondary, primary] : [primary];
  const runCount = axes.reduce((total, axis) => total * Math.max(axis.steps, 1), 1);
  const selected = METRICS.find((entry) => entry.key === metric)!;

  const run = async () => {
    setRunning(true);
    try {
      setResult(
        await api.sweep(
          twoDimensional ? "dashboard-2d" : "dashboard",
          baseline,
          axes.map((axis) => ({ parameter_path: axis.parameterPath, values: linspace(axis) })),
        ),
      );
    } catch (error) {
      onError(error instanceof Error ? error.message : String(error));
    } finally {
      setRunning(false);
    }
  };

  const isHeatMap = (result?.axes.length ?? 0) === 2;
  const lineData =
    result && !isHeatMap
      ? result.runs.map((row) => ({ x: row.coordinates[0], ...row }))
      : [];

  return (
    <div className="chart-card">
      <h3>Parameter sweep</h3>

      <div style={{ display: "flex", flexDirection: "column", gap: 8, marginBottom: 10 }}>
        <AxisControls axis={primary} onChange={setPrimary} parameters={parameters} label="axis 1" />
        {twoDimensional && (
          <AxisControls axis={secondary} onChange={setSecondary} parameters={parameters}
                        label="axis 2" />
        )}
        <div className="row" style={{ flexWrap: "wrap" }}>
          <label className="note" style={{ display: "flex", alignItems: "center", gap: 6 }}>
            <input type="checkbox" checked={twoDimensional}
                   onChange={(e) => setTwoDimensional(e.target.checked)} />
            second axis (heat map)
          </label>
          <button onClick={run} disabled={running || runCount > 400}>
            {running ? `Running ${runCount} shots…` : `Run sweep (${runCount})`}
          </button>
          {runCount > 400 && (
            <span className="note">
              This build runs sweeps synchronously; reduce to 400 runs or fewer.
            </span>
          )}
          {result && (
            <a href={api.csvUrl(result.sweep_id)} download={`${result.sweep_id}.csv`}>
              <button className="ghost">Download aggregate CSV</button>
            </a>
          )}
        </div>
      </div>

      {result && (
        <>
          <div className="row" style={{ marginBottom: 10 }}>
            <select style={{ width: 220 }} value={metric}
                    onChange={(e) => setMetric(e.target.value as HeatMetric)}>
              {METRICS.map((entry) => (
                <option key={entry.key} value={entry.key}>
                  {entry.label} ({entry.unit})
                </option>
              ))}
            </select>
            <span className="note">{result.run_count} runs · {result.status}</span>
          </div>

          {isHeatMap ? (
            <HeatMap
              rows={result.runs}
              yValues={result.axes[0].values}
              xValues={result.axes[1].values}
              yLabel={result.axes[0].parameter_path}
              xLabel={result.axes[1].parameter_path}
              metric={metric}
              metricLabel={selected.label}
              metricUnit={selected.unit}
            />
          ) : (
            <ResponsiveContainer width="100%" height={190}>
              <LineChart data={lineData} margin={{ top: 6, right: 14, bottom: 6, left: 4 }}>
                <CartesianGrid stroke="#3a302a" strokeDasharray="2 4" />
                <XAxis
                  dataKey="x" type="number" domain={["dataMin", "dataMax"]}
                  tick={{ fill: "#a2938a", fontSize: 11 }} stroke="#3a302a"
                  label={{ value: result.axes[0].parameter_path, position: "insideBottom",
                           offset: -3, fill: "#a2938a", fontSize: 11 }}
                />
                <YAxis
                  tick={{ fill: "#a2938a", fontSize: 11 }} stroke="#3a302a" width={62}
                  label={{ value: selected.unit, angle: -90, position: "insideLeft", offset: 12,
                           fill: "#a2938a", fontSize: 11 }}
                />
                <Tooltip
                  contentStyle={{ background: "#262019", border: "1px solid #3a302a", fontSize: 12 }}
                />
                <Line type="monotone" dataKey={metric} name={selected.label} stroke="#d98b4a"
                      strokeWidth={2} dot={{ r: 3 }} isAnimationActive={false} />
              </LineChart>
            </ResponsiveContainer>
          )}

          <table style={{ marginTop: 12 }}>
            <thead>
              <tr>
                {result.axes.map((axis) => (
                  <th key={axis.parameter_path}>{axis.parameter_path}</th>
                ))}
                <th>time (s)</th>
                <th>mass (g)</th>
                <th>TDS (%)</th>
                <th>yield (%)</th>
                <th>stop</th>
              </tr>
            </thead>
            <tbody>
              {result.runs.map((row) => (
                <tr key={row.index}>
                  {row.coordinates.map((value, index) => (
                    <td key={index}>{value}</td>
                  ))}
                  <td>{row.shot_time_s.toFixed(2)}</td>
                  <td>{row.beverage_mass_g.toFixed(1)}</td>
                  <td>{row.tds_percent.toFixed(2)}</td>
                  <td>{row.extraction_yield_percent.toFixed(2)}</td>
                  <td>{row.termination.replace(/_/g, " ")}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </>
      )}
    </div>
  );
}
