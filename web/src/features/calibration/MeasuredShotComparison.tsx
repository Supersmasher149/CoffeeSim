import { useEffect, useId, useRef, useState } from "react";

import { api } from "../../api/client";
import type {
  MeasuredShotCatalogue,
  MeasuredShotComparison as Comparison,
} from "../../api/types";
import { AnalysisCanvas, type CanvasSeries } from "../shared/AnalysisCanvas";
import { TRACE_STYLES } from "../../theme/traceStyles";

const DEFAULT_COEFFICIENT_SELECTOR = "default-v1";

function formatMetric(value: number, digits: number, unit: string) {
  return (
    <>
      {value.toFixed(digits)} <span className="unit">{unit}</span>
    </>
  );
}

// Audit #03: mass and residual used to be two independent recharts
// LineCharts with separate axes/tooltips, so reading a residual spike
// against the mass curve that produced it meant comparing two plots by eye.
// One AnalysisCanvas now carries both on a shared time axis and cursor.
function MeasuredComparisonCanvas({ pairedSeries }: { pairedSeries: Comparison["paired_series"] }) {
  // No PuckView-equivalent to sync with here, so this cursor is entirely
  // local to the canvas -- unlike ChartStack, which lifts it into
  // ShotWorkspace to also drive the puck animation.
  const [cursorTimeSeconds, setCursorTimeSeconds] = useState<number>();
  const measured: CanvasSeries = {
    key: "measured",
    label: "measured",
    color: TRACE_STYLES.measured.color,
    width: TRACE_STYLES.measured.width,
    format: (value) => `${value.toFixed(3)} g`,
    points: pairedSeries.map((row) => ({ time: row.time_s, value: row.measured_mass_g })),
  };
  const simulated: CanvasSeries = {
    key: "simulated",
    label: "simulated",
    color: TRACE_STYLES.simulated.color,
    width: TRACE_STYLES.simulated.width,
    dash: TRACE_STYLES.simulated.dash,
    format: (value) => `${value.toFixed(3)} g`,
    points: pairedSeries.map((row) => ({ time: row.time_s, value: row.simulated_mass_g })),
  };
  const residual: CanvasSeries = {
    key: "residual",
    label: "residual",
    color: TRACE_STYLES.residual.color,
    width: TRACE_STYLES.residual.width,
    format: (value) => `${value.toFixed(3)} g`,
    points: pairedSeries.map((row) => ({ time: row.time_s, value: row.residual_g })),
  };

  return (
    <>
      <div className="analysis-legend" aria-hidden="true">
        <span className="analysis-legend-item">
          <span className="analysis-legend-swatch" style={{ borderTopColor: measured.color, borderTopStyle: "solid" }} />
          measured
        </span>
        <span className="analysis-legend-item">
          <span className="analysis-legend-swatch" style={{ borderTopColor: simulated.color, borderTopStyle: "dashed" }} />
          simulated
        </span>
        <span className="analysis-legend-item">
          <span className="analysis-legend-swatch" style={{ borderTopColor: residual.color, borderTopStyle: "solid" }} />
          residual
        </span>
      </div>
      <AnalysisCanvas
        ariaLabel="Measured and simulated beverage mass, with the residual in its own lane, over measured sample time"
        series={[measured, simulated]}
        residual={residual}
        residualLabel="Residual · measured − simulated"
        cursorTimeSeconds={cursorTimeSeconds}
        onCursorChange={setCursorTimeSeconds}
      />
    </>
  );
}

// Audit #06: selection can be owned by a parent (GroundTruthList, so a
// measured-shot row in the merged ground-truth list drives this view) or, by
// default, entirely internally -- every existing standalone use and test
// keeps working unchanged.
interface Props {
  selectedId?: string;
  onSelectId?: (id: string) => void;
  onCatalogueLoaded?: (catalogue: MeasuredShotCatalogue) => void;
  hidePicker?: boolean;
}

export function MeasuredShotComparison({ selectedId: controlledId, onSelectId, onCatalogueLoaded, hidePicker = false }: Props = {}) {
  const selectId = useId();
  const [catalogue, setCatalogue] = useState<MeasuredShotCatalogue>();
  const [internalSelectedId, setInternalSelectedId] = useState("");
  const selectedId = controlledId ?? internalSelectedId;
  const setSelectedId = onSelectId ?? setInternalSelectedId;
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
        onCatalogueLoaded?.(body);
        if (controlledId === undefined) setSelectedId(body.measured_shots[0]?.id ?? "");
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
          {!hidePicker && (
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
          )}
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
            <MeasuredComparisonCanvas pairedSeries={comparison.paired_series} />
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
