import { useEffect, useId, useRef, useState } from "react";
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
  // Every field here was a bare <label> sibling with no htmlFor/id, so
  // neither a screen reader nor getByLabelText could associate "from" with
  // the axis it belonged to once there were two axes on the page.
  const parameterId = useId();
  const fromId = useId();
  const toId = useId();
  const stepsId = useId();
  return (
    <div className="row" style={{ flexWrap: "wrap", gap: 8 }}>
      <label className="note" htmlFor={parameterId} style={{ width: 54 }}>{label}</label>
      <select
        id={parameterId}
        style={{ width: 220 }}
        value={axis.parameterPath}
        onChange={(e) => onChange({ ...axis, parameterPath: e.target.value })}
      >
        {parameters.map((path) => (
          <option key={path} value={path}>{path}</option>
        ))}
      </select>
      <div className="field" style={{ margin: 0 }}>
        <label htmlFor={fromId}>from</label>
        <input id={fromId} type="number" value={axis.from}
               onChange={(e) => onChange({ ...axis, from: Number(e.target.value) })} />
      </div>
      <div className="field" style={{ margin: 0 }}>
        <label htmlFor={toId}>to</label>
        <input id={toId} type="number" value={axis.to}
               onChange={(e) => onChange({ ...axis, to: Number(e.target.value) })} />
      </div>
      <div className="field" style={{ margin: 0 }}>
        <label htmlFor={stepsId}>steps</label>
        <input id={stepsId} type="number" min={2} max={40} value={axis.steps}
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
  const [activeId, setActiveId] = useState<string>();
  const [submitting, setSubmitting] = useState(false);
  const pollTimer = useRef<number>();

  // The second axis is the outer one, so the primary axis varies fastest and
  // stays on the horizontal axis of the heat map.
  const axes = twoDimensional ? [secondary, primary] : [primary];
  const runCount = axes.reduce((total, axis) => total * Math.max(axis.steps, 1), 1);
  const selected = METRICS.find((entry) => entry.key === metric)!;
  const running = result?.status === "running" || result?.status === "queued";
  const busy = submitting || running;

  // Sweeps run in the background on the server, so the dashboard polls for
  // progress instead of holding a request open (15.2).
  useEffect(() => {
    if (!activeId) return undefined;

    let cancelled = false;
    const poll = async () => {
      try {
        const status = await api.sweepStatus(activeId);
        if (cancelled) return;
        setResult(status);
        if (status.status === "running" || status.status === "queued") {
          pollTimer.current = window.setTimeout(poll, 250);
        } else {
          setActiveId(undefined);
          if (status.status === "failed" && status.error) {
            onError(`${status.error.code}: ${status.error.message}`);
          }
        }
      } catch (error) {
        if (cancelled) return;
        setActiveId(undefined);
        onError(error instanceof Error ? error.message : String(error));
      }
    };
    poll();

    return () => {
      cancelled = true;
      window.clearTimeout(pollTimer.current);
    };
  }, [activeId, onError]);

  const run = async () => {
    if (busy) return;
    setSubmitting(true);
    try {
      const accepted = await api.startSweep(
        twoDimensional ? "dashboard-2d" : "dashboard",
        baseline,
        axes.map((axis) => ({ parameter_path: axis.parameterPath, values: linspace(axis) })),
      );
      setResult({
        sweep_id: accepted.sweep_id,
        status: accepted.status,
        completed: 0,
        total: accepted.total,
        elapsed_s: 0,
      });
      setActiveId(accepted.sweep_id);
    } catch (error) {
      onError(error instanceof Error ? error.message : String(error));
    } finally {
      setSubmitting(false);
    }
  };

  const cancel = async () => {
    if (!result) return;
    try {
      await api.cancelSweep(result.sweep_id);
    } catch (error) {
      onError(error instanceof Error ? error.message : String(error));
    }
  };

  const runs = result?.runs ?? [];
  const finished = result?.status === "complete" || result?.status === "cancelled";
  const isHeatMap = (result?.axes?.length ?? 0) === 2;
  const lineData =
    finished && !isHeatMap ? runs.map((row) => ({ x: row.coordinates[0], ...row })) : [];

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
          <button onClick={run} disabled={busy || runCount > 20000}>
            {submitting ? "Starting…" : running ? "Running…" : `Run sweep (${runCount})`}
          </button>
          {runCount > 20000 && (
            <span className="note">A single sweep is limited to 20000 runs.</span>
          )}
          {running && (
            <button className="ghost" onClick={cancel}>
              Cancel
            </button>
          )}
          {finished && result && (
            <a className="button ghost" href={api.csvUrl(result.sweep_id)} download={`${result.sweep_id}.csv`}>
              Download aggregate CSV
            </a>
          )}
        </div>
      </div>

      {running && result && (
        <div style={{ marginBottom: 12 }}>
          <div
            role="progressbar"
            aria-label="Sweep progress"
            aria-valuemin={0}
            aria-valuemax={result.total}
            aria-valuenow={result.completed}
            aria-valuetext={`${result.completed} of ${result.total} runs`}
            style={{
              height: 6, borderRadius: 3, background: "var(--bg)",
              border: "1px solid var(--line)", overflow: "hidden",
            }}
          >
            <div
              style={{
                height: "100%",
                width: `${result.total ? (result.completed / result.total) * 100 : 0}%`,
                background: "var(--accent)",
                transition: "width 200ms linear",
              }}
            />
          </div>
          <div className="note" style={{ marginTop: 5 }}>
            {result.completed} / {result.total} runs · {result.elapsed_s.toFixed(1)} s
            {result.completed > 0 &&
              ` · ${Math.round(result.completed / Math.max(result.elapsed_s, 0.001))} runs/s`}
          </div>
        </div>
      )}

      {finished && result && (
        <>
          <div className="row" style={{ marginBottom: 10 }}>
            <select style={{ width: 220 }} value={metric} aria-label="Metric"
                    onChange={(e) => setMetric(e.target.value as HeatMetric)}>
              {METRICS.map((entry) => (
                <option key={entry.key} value={entry.key}>
                  {entry.label} ({entry.unit})
                </option>
              ))}
            </select>
            <span className="note">
              {result.run_count} runs · {result.status}
              {result.cancelled && " (partial results kept)"} · {result.elapsed_s.toFixed(2)} s
            </span>
          </div>

          {isHeatMap ? (
            <HeatMap
              rows={runs}
              yValues={result.axes![0].values}
              xValues={result.axes![1].values}
              yLabel={result.axes![0].parameter_path}
              xLabel={result.axes![1].parameter_path}
              metric={metric}
              metricLabel={selected.label}
              metricUnit={selected.unit}
              partial={result.cancelled === true}
            />
          ) : (
            <div
              role="img"
              aria-label={
                `${selected.label} against ${result.axes![0].parameter_path} across ${runs.length} runs.`
              }
            >
            <ResponsiveContainer width="100%" height={190}>
              <LineChart data={lineData} margin={{ top: 6, right: 14, bottom: 6, left: 4 }}>
                <CartesianGrid stroke="#3a302a" strokeDasharray="2 4" />
                <XAxis
                  dataKey="x" type="number" domain={["dataMin", "dataMax"]}
                  tick={{ fill: "#a2938a", fontSize: 11 }} stroke="#3a302a"
                  label={{ value: result.axes![0].parameter_path, position: "insideBottom",
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
            </div>
          )}

          <div className="table-scroll">
          <table style={{ marginTop: 12 }}>
            <thead>
              <tr>
                {result.axes!.map((axis) => (
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
              {runs.map((row) => (
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
          </div>
        </>
      )}
    </div>
  );
}
