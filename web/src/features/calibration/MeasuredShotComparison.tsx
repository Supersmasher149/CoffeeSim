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
import type {
  MeasuredShotCatalogue,
  MeasuredShotComparison as Comparison,
} from "../../api/types";

const DEFAULT_COEFFICIENT_SELECTOR = "default-v1";

function formatMetric(value: number, digits: number, unit: string) {
  return (
    <>
      {value.toFixed(digits)} <span className="unit">{unit}</span>
    </>
  );
}

function ComparisonTooltip({ active, payload, label }: any) {
  if (!active || !payload?.length) return null;
  return (
    <div className="tooltip">
      <div className="t">t = {Number(label).toFixed(2)} s</div>
      {payload.map((entry: any) => (
        <div key={entry.dataKey} style={{ color: entry.color }}>
          {entry.name}: {Number(entry.value).toFixed(3)} g
        </div>
      ))}
    </div>
  );
}

function ComparisonChart({
  title,
  data,
  residual = false,
}: {
  title: string;
  data: Comparison["paired_series"];
  residual?: boolean;
}) {
  return (
    <div className="comparison-chart" role="img" aria-label={`${title} over measured sample time`}>
      <h4>{title}</h4>
      <ResponsiveContainer width="100%" height={210}>
        <LineChart data={data} margin={{ top: 6, right: 14, bottom: 8, left: 4 }}>
          <CartesianGrid stroke="#3a302a" strokeDasharray="2 4" />
          <XAxis
            dataKey="time_s"
            type="number"
            domain={["dataMin", "dataMax"]}
            tick={{ fill: "#a2938a", fontSize: 11 }}
            stroke="#3a302a"
            label={{
              value: "measured sample time (s)",
              position: "insideBottom",
              offset: -5,
              fill: "#a2938a",
              fontSize: 11,
            }}
          />
          <YAxis
            tick={{ fill: "#a2938a", fontSize: 11 }}
            stroke="#3a302a"
            width={58}
            label={{
              value: residual ? "residual (g)" : "mass (g)",
              angle: -90,
              position: "insideLeft",
              offset: 12,
              fill: "#a2938a",
              fontSize: 11,
            }}
          />
          <Tooltip content={<ComparisonTooltip />} />
          {residual ? (
            <Line
              type="linear"
              dataKey="residual_g"
              name="measured - simulated"
              stroke="#d9584a"
              strokeWidth={1.8}
              dot={false}
              isAnimationActive={false}
            />
          ) : (
            <>
              <Line
                type="linear"
                dataKey="measured_mass_g"
                name="measured"
                stroke="#6fb3c8"
                strokeWidth={2}
                dot={{ r: 2 }}
                isAnimationActive={false}
              />
              <Line
                type="linear"
                dataKey="simulated_mass_g"
                name="simulated"
                stroke="#d98b4a"
                strokeWidth={1.8}
                strokeDasharray="5 3"
                dot={false}
                isAnimationActive={false}
              />
            </>
          )}
        </LineChart>
      </ResponsiveContainer>
    </div>
  );
}

export function MeasuredShotComparison() {
  const selectId = useId();
  const [catalogue, setCatalogue] = useState<MeasuredShotCatalogue>();
  const [selectedId, setSelectedId] = useState("");
  const [catalogueError, setCatalogueError] = useState<string>();
  const [comparison, setComparison] = useState<Comparison>();
  const [comparisonError, setComparisonError] = useState<string>();
  const [loadingComparison, setLoadingComparison] = useState(false);
  const comparisonController = useRef<AbortController>();
  const requestGeneration = useRef(0);

  useEffect(() => {
    const controller = new AbortController();
    api
      .measuredShots(controller.signal)
      .then((body) => {
        setCatalogue(body);
        setSelectedId(body.measured_shots[0]?.id ?? "");
      })
      .catch((failure) => {
        if (failure instanceof DOMException && failure.name === "AbortError") return;
        setCatalogueError(failure instanceof Error ? failure.message : String(failure));
      });
    return () => controller.abort();
  }, []);

  useEffect(
    () => () => {
      requestGeneration.current += 1;
      comparisonController.current?.abort();
    },
    [],
  );

  const compare = async () => {
    if (!selectedId) return;
    comparisonController.current?.abort();
    const controller = new AbortController();
    comparisonController.current = controller;
    const generation = ++requestGeneration.current;
    setLoadingComparison(true);
    setComparisonError(undefined);

    try {
      const result = await api.compareMeasuredShot(
        selectedId,
        DEFAULT_COEFFICIENT_SELECTOR,
        controller.signal,
      );
      if (requestGeneration.current === generation) setComparison(result);
    } catch (failure) {
      if (requestGeneration.current !== generation) return;
      if (failure instanceof DOMException && failure.name === "AbortError") return;
      setComparisonError(failure instanceof Error ? failure.message : String(failure));
    } finally {
      if (requestGeneration.current === generation) setLoadingComparison(false);
    }
  };

  const selectShot = (id: string) => {
    requestGeneration.current += 1;
    comparisonController.current?.abort();
    setSelectedId(id);
    setComparison(undefined);
    setComparisonError(undefined);
    setLoadingComparison(false);
  };

  return (
    <section className="chart-card measured-comparison" aria-labelledby="measured-comparison-title">
      <div className="measured-comparison-heading">
        <div>
          <h3 id="measured-comparison-title">Measured-shot comparison</h3>
          <p className="note">
            Run one native simulation against one recorded mass series. Values, interpolation, and
            loss metrics come from the server.
          </p>
        </div>
        <span className="coefficient-badge">{DEFAULT_COEFFICIENT_SELECTOR}</span>
      </div>

      {catalogueError && (
        <div className="error" role="alert">
          Measured-shot catalogue unavailable: {catalogueError}
        </div>
      )}
      {!catalogue && !catalogueError && (
        <p className="note" aria-live="polite">Loading measured-shot catalogue...</p>
      )}
      {catalogue && catalogue.measured_shots.length === 0 && (
        <p className="note">No valid measured shots are available.</p>
      )}
      {catalogue && catalogue.measured_shots.length > 0 && (
        <div className="comparison-controls">
          <div>
            <label htmlFor={selectId}>Measured shot</label>
            <select id={selectId} value={selectedId} onChange={(event) => selectShot(event.target.value)}>
              {catalogue.measured_shots.map((shot) => (
                <option value={shot.id} key={shot.id}>
                  {shot.id}{shot.synthetic ? " (synthetic)" : " (real)"}
                </option>
              ))}
            </select>
          </div>
          <button type="button" onClick={compare} disabled={!selectedId || loadingComparison}>
            {loadingComparison ? "Comparing..." : "Compare with default-v1"}
          </button>
        </div>
      )}

      <div className="comparison-status" aria-live="polite">
        {loadingComparison && "Running the native comparison..."}
      </div>
      {comparisonError && <div className="error" role="alert">Comparison failed: {comparisonError}</div>}

      {comparison && (
        <>
          <div className={`comparison-warning ${comparison.synthetic ? "synthetic" : "real"}`}>
            {comparison.synthetic
              ? "Synthetic fixture — generated from the simulator with added scale noise. This verifies the comparison workflow, not real-world model accuracy."
              : "Recorded measurement — comparison results depend on the documented setup and selected coefficient set. A single shot is not sufficient to validate the model."}
          </div>

          <p className="comparison-coefficient-note">
            Compared against uncalibrated default-v1 coefficients.
          </p>

          <div className="comparison-provenance kv">
            <span className="k">Shot</span><span>{comparison.id}</span>
            <span className="k">Source</span><span>{comparison.source_stem}</span>
            <span className="k">Machine</span><span>{comparison.machine || "Not reported"}</span>
            <span className="k">Coefficient selector</span><span>{comparison.coefficients.selector}</span>
            <span className="k">Coefficient</span><span>{comparison.coefficients.id} v{comparison.coefficients.version}</span>
            <span className="k">Coefficient hash</span><code>{comparison.coefficients.hash}</code>
            <span className="k">Simulation status</span><span>{comparison.loss.simulated ? "Completed" : "Failed"}</span>
            <span className="k">Termination</span><span>{comparison.simulation.termination}</span>
            <span className="k">Result hash</span><code>{comparison.simulation.result_hash}</code>
          </div>

          <div className="metric-strip comparison-metrics">
            <div className="metric">
              <div className="label">Mass RMSE</div>
              <div className="value">{formatMetric(comparison.loss.mass_rmse_g, 3, "g")}</div>
            </div>
            <div className="metric">
              <div className="label">Time error</div>
              <div className="value">
                {comparison.loss.has_time_measurement
                  ? formatMetric(comparison.loss.time_error_s, 2, "s")
                  : "Not measured"}
              </div>
            </div>
            <div className="metric">
              <div className="label">TDS error</div>
              <div className="value">
                {comparison.loss.has_tds_measurement
                  ? formatMetric(comparison.loss.tds_error_percent, 3, "%")
                  : "Not measured"}
              </div>
            </div>
            <div className="metric diagnostic">
              <div className="label">Pressure RMSE (diagnostic only)</div>
              <div className="value">
                {comparison.loss.has_pressure_measurement
                  ? formatMetric(comparison.loss.pressure_rmse_bar, 3, "bar")
                  : "Not measured"}
              </div>
            </div>
            <div className="metric">
              <div className="label">Weighted loss</div>
              <div className="value">{comparison.loss.total.toFixed(3)}</div>
            </div>
          </div>

          {comparison.paired_series.length > 0 ? (
            <div className="comparison-charts">
              <ComparisonChart title="Measured and simulated beverage mass" data={comparison.paired_series} />
              <ComparisonChart title="Beverage-mass residual" data={comparison.paired_series} residual />
            </div>
          ) : (
            <p className="note">This shot has no paired mass-series points to plot.</p>
          )}
        </>
      )}

      <details className="comparison-cli-details">
        <summary>Fit coefficients with the existing CLI calibration workflow</summary>
        <p className="note">
          The calibration workflow lives in the CLI and in <code>assets/measured_shots/</code>:
          record a baseline shot, minimise the weighted error across several shots, hold one shot
          back as a validation case, and commit the fitted coefficient file with its dataset
          reference and limitations.
        </p>
      </details>
    </section>
  );
}
