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

interface Props {
  baseline: Recipe;
  parameters: string[];
  onError: (message: string) => void;
}

function linspace(from: number, to: number, steps: number): number[] {
  if (steps < 2) return [from];
  return Array.from({ length: steps }, (_, i) => from + ((to - from) * i) / (steps - 1));
}

// Section 12.3, experiment view: sweep definition, progress, run selection.
// The MVP runs sweeps synchronously (15.2), so "progress" is a busy state.
export function SweepPanel({ baseline, parameters, onError }: Props) {
  const [parameterPath, setParameterPath] = useState("puck.particle_diameter_um");
  const [from, setFrom] = useState(250);
  const [to, setTo] = useState(450);
  const [steps, setSteps] = useState(9);
  const [metric, setMetric] = useState<"shot_time_s" | "extraction_yield_percent" | "tds_percent">(
    "shot_time_s",
  );
  const [result, setResult] = useState<SweepResult>();
  const [running, setRunning] = useState(false);

  const run = async () => {
    setRunning(true);
    try {
      setResult(await api.sweep("dashboard", baseline, parameterPath, linspace(from, to, steps)));
    } catch (error) {
      onError(error instanceof Error ? error.message : String(error));
    } finally {
      setRunning(false);
    }
  };

  const chartData =
    result?.runs.map((run_) => ({
      x: run_.coordinates[0],
      shot_time_s: run_.shot_time_s,
      extraction_yield_percent: run_.extraction_yield_percent,
      tds_percent: run_.tds_percent,
    })) ?? [];

  const metricUnit = metric === "shot_time_s" ? "s" : "%";

  return (
    <div className="chart-card">
      <h3>Parameter sweep</h3>
      <div className="row" style={{ flexWrap: "wrap", marginBottom: 10 }}>
        <select
          style={{ width: 230 }}
          value={parameterPath}
          onChange={(e) => setParameterPath(e.target.value)}
        >
          {parameters.map((path) => (
            <option key={path} value={path}>
              {path}
            </option>
          ))}
        </select>
        <div className="field" style={{ margin: 0 }}>
          <label>from</label>
          <input type="number" value={from} onChange={(e) => setFrom(Number(e.target.value))} />
        </div>
        <div className="field" style={{ margin: 0 }}>
          <label>to</label>
          <input type="number" value={to} onChange={(e) => setTo(Number(e.target.value))} />
        </div>
        <div className="field" style={{ margin: 0 }}>
          <label>steps</label>
          <input
            type="number" min={2} max={200} value={steps}
            onChange={(e) => setSteps(Number(e.target.value))}
          />
        </div>
        <button onClick={run} disabled={running}>
          {running ? `Running ${steps} shots…` : "Run sweep"}
        </button>
        {result && (
          <a href={api.csvUrl(result.sweep_id)} download={`${result.sweep_id}.csv`}>
            <button className="ghost">Download aggregate CSV</button>
          </a>
        )}
      </div>

      {result && (
        <>
          <div className="row" style={{ marginBottom: 8 }}>
            <select
              style={{ width: 220 }}
              value={metric}
              onChange={(e) => setMetric(e.target.value as typeof metric)}
            >
              <option value="shot_time_s">shot time (s)</option>
              <option value="extraction_yield_percent">extraction yield (%)</option>
              <option value="tds_percent">TDS (%)</option>
            </select>
            <span className="note">
              {result.run_count} runs · {result.status}
            </span>
          </div>

          <ResponsiveContainer width="100%" height={190}>
            <LineChart data={chartData} margin={{ top: 6, right: 14, bottom: 6, left: 4 }}>
              <CartesianGrid stroke="#3a302a" strokeDasharray="2 4" />
              <XAxis
                dataKey="x" type="number" domain={["dataMin", "dataMax"]}
                tick={{ fill: "#a2938a", fontSize: 11 }} stroke="#3a302a"
                label={{ value: parameterPath, position: "insideBottom", offset: -3,
                         fill: "#a2938a", fontSize: 11 }}
              />
              <YAxis
                tick={{ fill: "#a2938a", fontSize: 11 }} stroke="#3a302a" width={62}
                label={{ value: metricUnit, angle: -90, position: "insideLeft", offset: 12,
                         fill: "#a2938a", fontSize: 11 }}
              />
              <Tooltip
                contentStyle={{ background: "#262019", border: "1px solid #3a302a", fontSize: 12 }}
              />
              <Line
                type="monotone" dataKey={metric} stroke="#d98b4a" strokeWidth={2}
                dot={{ r: 3 }} isAnimationActive={false}
              />
            </LineChart>
          </ResponsiveContainer>

          <table style={{ marginTop: 10 }}>
            <thead>
              <tr>
                <th>{parameterPath}</th>
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
                  <td>{row.coordinates[0].toFixed(1)}</td>
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
