import type { ShotResult } from "../../api/types";

function Metric({ label, value, unit }: { label: string; value: string; unit?: string }) {
  return (
    <div className="metric">
      <div className="label">{label}</div>
      <div className="value">
        {value}
        {unit && <span className="unit">{unit}</span>}
      </div>
    </div>
  );
}

// Section 12.3, metric strip.
export function MetricStrip({ result }: { result: ShotResult }) {
  return (
    <div className="metric-strip">
      <Metric label="Shot time" value={result.elapsed_time_s.toFixed(1)} unit="s" />
      <Metric label="Beverage" value={result.beverage_mass_g.toFixed(1)} unit="g" />
      <Metric label="Avg flow" value={result.average_flow_ml_s.toFixed(2)} unit="ml/s" />
      <Metric label="TDS" value={result.tds_percent.toFixed(2)} unit="%" />
      <Metric label="Extraction" value={result.extraction_yield_percent.toFixed(2)} unit="%" />
      <Metric label="Brew ratio" value={`1:${result.brew_ratio.toFixed(2)}`} />
      <Metric label="Stop" value={result.termination.replace(/_/g, " ")} />
    </div>
  );
}
