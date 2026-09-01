import type { ShotResult } from "../../api/types";
import { AnalysisCanvas, type CanvasSeries } from "../shared/AnalysisCanvas";
import { overlayDashFor, TRACE_STYLES } from "../../theme/traceStyles";

interface Props {
  result: ShotResult;
  comparisons: ShotResult[];
  preInfusionEnd?: number;
  cursorTimeSeconds?: number;
  onCursorChange: (time?: number) => void;
}

interface Channel {
  key: "pressure" | "flow" | "mass" | "temperature";
  label: string;
  unit: string;
  extract: (sample: ShotResult["samples"][number]) => number;
}

// The four traces the Hi-Fi legend names (pressure/flow/mass/temperature),
// always drawn together -- audit #04 retired the single-signal <select> that
// used to gate the other three off screen.
const CHANNELS: Channel[] = [
  { key: "pressure", label: "pressure", unit: " bar", extract: (s) => s.pressure_bar },
  { key: "flow", label: "flow", unit: " ml/s", extract: (s) => s.flow_ml_s },
  { key: "mass", label: "mass", unit: " g", extract: (s) => s.beverage_mass_g },
  { key: "temperature", label: "temperature", unit: " °C", extract: (s) => s.puck_temperature_c },
];

const MAX_RUNS = 3;

export function ChartStack({ result, comparisons, preInfusionEnd, cursorTimeSeconds, onCursorChange }: Props) {
  const runs = [result, ...comparisons].slice(0, MAX_RUNS);
  const overlayRuns = runs.slice(1);

  const series: CanvasSeries[] = CHANNELS.map((channel) => {
    const style = TRACE_STYLES[channel.key];
    return {
      key: channel.key,
      label: channel.label,
      color: style.color,
      width: style.width,
      dash: style.dash,
      format: (value) => `${value.toFixed(2)}${channel.unit}`,
      points: result.samples.map((sample) => ({ time: sample.time_s, value: channel.extract(sample) })),
    };
  });

  const overlaySeries: CanvasSeries[] = overlayRuns.flatMap((run) => {
    const dash = overlayDashFor(run.manifest.run_id);
    const tag = run.manifest.run_id.slice(5, 11);
    return CHANNELS.map((channel) => {
      const style = TRACE_STYLES[channel.key];
      return {
        key: `${channel.key}_${run.manifest.run_id}`,
        label: `${channel.label} · ${tag}`,
        color: style.color,
        width: 1.4,
        dash,
        opacity: 0.7,
        format: (value: number) => `${value.toFixed(2)}${channel.unit}`,
        points: run.samples.map((sample) => ({ time: sample.time_s, value: channel.extract(sample) })),
      };
    });
  });

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

  const ariaLabel =
    `Shot analysis: ${CHANNELS.map((c) => c.label).join(", ")} over the shot's timeline` +
    (overlayRuns.length > 0
      ? `, overlaid with ${overlayRuns.length} pinned run${overlayRuns.length > 1 ? "s" : ""} (${overlayRuns
          .map((run) => run.manifest.run_id.slice(5, 11))
          .join(", ")})`
      : "") +
    (markers.length > 0 ? `, with ${markers.length} marked event${markers.length === 1 ? "" : "s"}.` : ".");

  return (
    <section className="analysis-surface" aria-labelledby="shot-analysis-title">
      <div className="analysis-heading">
        <div>
          <p className="eyebrow">Synchronized timeline</p>
          <h2 id="shot-analysis-title">Shot analysis</h2>
        </div>
      </div>

      <div className="analysis-legend" aria-hidden="true">
        {CHANNELS.map((channel) => {
          const style = TRACE_STYLES[channel.key];
          return (
            <span key={channel.key} className="analysis-legend-item">
              <span
                className="analysis-legend-swatch"
                style={{ borderTopColor: style.color, borderTopStyle: style.dash ? "dashed" : "solid" }}
              />
              {channel.label}
              {channel.unit}
            </span>
          );
        })}
      </div>

      <div className="event-lane" aria-label="Shot timeline events">
        {markers.length > 0 ? markers.map((marker, index) => (
          <span key={`${marker.label}-${marker.time}-${index}`} style={{ borderColor: marker.color }}>
            <b>{marker.time.toFixed(1)} s</b> {marker.label}
          </span>
        )) : <span>No warnings or stop events were recorded.</span>}
      </div>

      <AnalysisCanvas
        ariaLabel={ariaLabel}
        series={[...series, ...overlaySeries]}
        markers={markers}
        preInfusionEnd={preInfusionEnd}
        cursorTimeSeconds={cursorTimeSeconds}
        onCursorChange={onCursorChange}
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
