import type { ShotResult } from "../../api/types";

// Section 12.3: warnings, clamps, residuals, versions and model assumptions.
export function DiagnosticsDrawer({ result }: { result: ShotResult }) {
  const { diagnostics, manifest } = result;

  return (
    <details className="drawer">
      <summary>
        Diagnostics — {result.warnings.length} warning{result.warnings.length === 1 ? "" : "s"},{" "}
        {diagnostics.clamp_count} clamp{diagnostics.clamp_count === 1 ? "" : "s"}
      </summary>
      <div className="body">
        {result.warnings.length === 0 ? (
          <p className="note">No warnings: nothing was clamped and no invariant was stressed.</p>
        ) : (
          result.warnings.map((warning, index) => (
            <div className={`warning ${warning.severity}`} key={`${warning.code}-${index}`}>
              <div>
                <code>{warning.code}</code> at {warning.time_s.toFixed(2)} s
                <div>{warning.message}</div>
              </div>
            </div>
          ))
        )}

        <h3 style={{ fontSize: 12, color: "var(--muted)", margin: "14px 0 6px" }}>
          Mass balance residuals
        </h3>
        <div className="kv">
          <span className="k">water</span>
          <span>{diagnostics.water_mass_residual_g.toExponential(2)} g</span>
          <span className="k">dissolved solids</span>
          <span>{diagnostics.solids_mass_residual_g.toExponential(2)} g</span>
          <span className="k">solver steps</span>
          <span>{diagnostics.step_count}</span>
          <span className="k">min permeability</span>
          <span>{diagnostics.min_permeability_m2.toExponential(2)} m²</span>
          <span className="k">puck temperature</span>
          <span>
            {diagnostics.min_puck_temperature_c.toFixed(1)} to{" "}
            {diagnostics.max_puck_temperature_c.toFixed(1)} °C
          </span>
        </div>

        <h3 style={{ fontSize: 12, color: "var(--muted)", margin: "14px 0 6px" }}>
          Versions and reproducibility
        </h3>
        <div className="kv">
          <span className="k">run id</span>
          <span>{manifest.run_id}</span>
          <span className="k">solver</span>
          <span>{manifest.solver_version}</span>
          <span className="k">coefficients</span>
          <span>
            {manifest.coefficient_id} v{manifest.coefficient_version}
          </span>
          <span className="k">recipe hash</span>
          <span>{manifest.recipe_hash.slice(0, 16)}…</span>
          <span className="k">result hash</span>
          <span>{manifest.result_hash.slice(0, 16)}…</span>
          <span className="k">step / sample</span>
          <span>
            {manifest.dt_s} s / {manifest.sample_interval_s} s
          </span>
        </div>

        <h3 style={{ fontSize: 12, color: "var(--muted)", margin: "14px 0 6px" }}>
          Model assumptions
        </h3>
        <p className="note">
          One-dimensional lumped puck: uniform flow, one thermal mass, no channelling and no
          spatial temperature or concentration field. Estimated TDS and extraction yield are
          engineering outputs, not flavour scores — water chemistry and distribution are not
          resolved (8.5). When a bean is attached, the sensory panel adds a heuristic estimate
          computed from authored solute-class priors that have never been compared with a tasting
          panel; it changes no quantity above. The coefficient set above is uncalibrated until a
          measured shot has been fitted.
        </p>
      </div>
    </details>
  );
}
