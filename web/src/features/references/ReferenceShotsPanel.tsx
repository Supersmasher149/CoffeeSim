import type {
  Recipe,
  ReferenceCatalogue,
  ReferenceRecord,
  ShotResult,
} from "../../api/types";

interface Props {
  catalogue?: ReferenceCatalogue;
  active?: ShotResult;
  recipe?: Recipe;
  error?: string;
}

function value(value: number | null | undefined, digits: number, unit = "") {
  return value == null ? "not reported" : `${value.toFixed(digits)}${unit}`;
}

function referenceLabel(reference: ReferenceRecord) {
  return reference.id.replace(/^real_gagne_/, "").replace(/_/g, " ").toUpperCase();
}

function modelValue(
  row: string,
  active: ShotResult | undefined,
  recipe: Recipe | undefined,
) {
  if (!active) return "";
  switch (row) {
    case "Dose":
      return recipe ? value(recipe.puck.dose_g, 1, " g") : "not reported";
    case "Beverage mass":
      return value(active.beverage_mass_g, 1, " g");
    case "Shot time":
      return value(active.elapsed_time_s, 1, " s");
    case "Brew ratio":
      return `1:${active.brew_ratio.toFixed(2)}`;
    case "Filtered TDS":
      return value(active.tds_percent, 2, "%");
    case "Filtered extraction":
      return value(active.extraction_yield_percent, 2, "%");
    default:
      return "not reported";
  }
}

const metricRows = [
  {
    label: "Dose",
    reference: (shot: ReferenceRecord) => value(shot.observed.dose_g, 1, " g"),
  },
  {
    label: "Beverage mass",
    reference: (shot: ReferenceRecord) => value(shot.observed.final_beverage_mass_g, 1, " g"),
  },
  {
    label: "Shot time",
    reference: (shot: ReferenceRecord) => value(shot.observed.final_shot_time_s, 1, " s"),
  },
  {
    label: "Brew ratio",
    reference: (shot: ReferenceRecord) => `1:${shot.setup.target_brew_ratio.toFixed(1)} target`,
  },
  {
    label: "Peak pressure",
    reference: (shot: ReferenceRecord) => value(shot.observed.peak_pressure_bar, 1, " bar"),
  },
  {
    label: "Filtered TDS",
    reference: (shot: ReferenceRecord) => value(shot.observed.tds_filtered_pct, 2, "%"),
  },
  {
    label: "Filtered extraction",
    reference: (shot: ReferenceRecord) => value(shot.observed.extraction_yield_filtered_pct, 2, "%"),
  },
  {
    label: "Drip mass",
    reference: (shot: ReferenceRecord) => value(shot.observed.drip_g, 1, " g"),
  },
];

function SourceLinks({ reference }: { reference: ReferenceRecord }) {
  return (
    <span className="reference-links">
      <a href={reference.source.article_url} target="_blank" rel="noreferrer">
        article
      </a>
      <a href={reference.source.experiment_log_url} target="_blank" rel="noreferrer">
        experiment log
      </a>
    </span>
  );
}

function SetupDetails({ reference }: { reference: ReferenceRecord }) {
  const coffee = reference.setup.coffee;
  return (
    <details className="reference-card">
      <summary>
        {referenceLabel(reference)} · {coffee.name}
      </summary>
      <div className="kv">
        <span className="k">Machine</span><span>{reference.setup.machine}</span>
        <span className="k">Coffee</span><span>{coffee.origin} · {coffee.process}</span>
        <span className="k">Varieties</span><span>{coffee.varieties.join(", ")}</span>
        <span className="k">Profile</span><span>{reference.setup.profile}</span>
        <span className="k">Bloom</span><span>{value(reference.setup.bloom_time_s, 0, " s")}</span>
        <span className="k">Source shot</span><span>{reference.source.de1_shot_file}</span>
      </div>
    </details>
  );
}

export function ReferenceShotsPanel({ catalogue, active, recipe, error }: Props) {
  return (
    <section className="chart-card reference-panel">
      <div className="reference-heading">
        <div>
          <h3>Real-world references</h3>
          <p className="note">
            Reported shot metadata for context. These records are not calibration data or telemetry
            traces.
          </p>
        </div>
        <span className="reference-badge">metadata only</span>
      </div>

      {error && <div className="error" role="alert">Reference catalogue unavailable: {error}</div>}
      {!catalogue && !error && <p className="note" aria-live="polite">Loading reference catalogue…</p>}
      {catalogue && catalogue.references.length === 0 && (
        <p className="note">No valid reference records are available.</p>
      )}
      {catalogue && catalogue.references.length > 0 && (
        <>
          <div className="reference-warning">
            {catalogue.limitation} Comparison with the current model is contextual only: the
            recipes and setup differ, and reference shot time is unavailable.
          </div>
          <div className="reference-table-wrap">
            <table className="reference-table">
              <thead>
                <tr>
                  <th scope="col">Metric</th>
                  {active && <th scope="col" className="current-model">Current model</th>}
                  {catalogue.references.map((reference) => (
                    <th scope="col" key={reference.id}>
                      <strong>{referenceLabel(reference)}</strong>
                      <span className="reference-column-subtitle">
                        {reference.grinder.model} · setting {reference.grinder.setting}
                      </span>
                      <SourceLinks reference={reference} />
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {metricRows.map((row) => (
                  <tr key={row.label}>
                    <th scope="row">{row.label}</th>
                    {active && (
                      <td className="current-model">{modelValue(row.label, active, recipe)}</td>
                    )}
                    {catalogue.references.map((reference) => (
                      <td key={reference.id}>{row.reference(reference)}</td>
                    ))}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          <div className="reference-details">
            {catalogue.references.map((reference) => (
              <SetupDetails reference={reference} key={reference.id} />
            ))}
          </div>
        </>
      )}
      {catalogue && catalogue.load_errors.length > 0 && (
        <div className="reference-load-errors" role="status">
          {catalogue.load_errors.map((loadError) => (
            <div key={`${loadError.file}-${loadError.code}`}>
              <code>{loadError.code}</code> {loadError.file}: {loadError.message}
            </div>
          ))}
        </div>
      )}
    </section>
  );
}
