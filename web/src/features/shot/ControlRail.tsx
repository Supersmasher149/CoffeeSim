import { useId } from "react";

import { SOLUTE_CLASSES } from "../../api/types";
import type {
  BeanProfile,
  GrindBin,
  Recipe,
  RecipeCatalogueEntry,
  ValidationIssue,
} from "../../api/types";
import { inputRanges } from "../../state/workspace";
import { ProfileEditor } from "./ProfileEditor";

interface Props {
  recipes: RecipeCatalogueEntry[];
  selectedId: string;
  onSelectRecipe: (id: string) => void;
  recipe: Recipe;
  onChange: (recipe: Recipe) => void;
  issues: ValidationIssue[];
  running: boolean;
  onRun: () => void;
}

interface NumberFieldProps {
  label: string;
  value: number;
  range: readonly [number, number];
  step?: number;
  path: string;
  issues: ValidationIssue[];
  onChange: (value: number) => void;
}

// Renders the authored distribution and nothing else. The representative
// diameter (d32) and the spread penalty are derived from these bins by the
// native solver, so they are deliberately NOT recomputed here: per CLAUDE.md
// the dashboard renders model outputs and never re-derives an authoritative
// quantity in the browser. Bar widths are display-only presentation.
function GrindDistributionReadout({ bins }: { bins: GrindBin[] }) {
  const peak = bins.reduce((max, bin) => Math.max(max, bin.mass_fraction), 0) || 1;
  return (
    <div className="grind-psd">
      <ul className="grind-psd-bins">
        {bins.map((bin) => (
          <li key={bin.diameter_um}>
            <span className="grind-psd-label">{bin.diameter_um} µm</span>
            <span className="grind-psd-bar" aria-hidden="true">
              <span style={{ width: `${(bin.mass_fraction / peak) * 100}%` }} />
            </span>
            <span className="grind-psd-value">{(bin.mass_fraction * 100).toFixed(1)}%</span>
          </li>
        ))}
      </ul>
      <p className="note">
        This recipe carries a particle size distribution rather than a single diameter, so the
        scalar controls do not apply. The solver derives the representative diameter (Sauter mean,
        d32) and the spread penalty from these bins, and extracts each size class at its own rate.
        Edit the recipe file to change the distribution.
      </p>
    </div>
  );
}

// Units live in the label, never only in surrounding prose (12.5).
function NumberField({ label, value, range, step = 1, path, issues, onChange }: NumberFieldProps) {
  const invalid = issues.some((issue) => issue.path === path);
  // The label and input were siblings with no htmlFor/id link, so a screen
  // reader (and RTL's getByLabelText) had no way to associate one with the
  // other. useId keeps it unique across the rail's several NumberFields.
  const inputId = useId();
  return (
    <div className="field">
      <label htmlFor={inputId} title={`${range[0]} to ${range[1]}`}>{label}</label>
      <input
        id={inputId}
        type="number"
        className={invalid ? "invalid" : undefined}
        aria-invalid={invalid}
        value={value}
        step={step}
        min={range[0]}
        max={range[1]}
        onChange={(event) => onChange(Number(event.target.value))}
      />
    </div>
  );
}

// Read-only, like GrindDistributionReadout above. There is deliberately no bean
// editor: these are authored priors, not brew controls, and offering sliders
// over them would suggest a precision that does not exist. Bean documents are
// edited by hand under assets/beans/ or supplied with `simulate --bean`.
function BeanReadout({ bean }: { bean: BeanProfile }) {
  const description = bean.description;
  return (
    <div className="bean-readout">
      <div className="bean-name">{description?.display_name ?? bean.id}</div>
      {description?.roaster && <div className="note">{description.roaster}</div>}
      {description?.notes && description.notes.length > 0 && (
        <div className="note">{description.notes.join(" · ")}</div>
      )}
      <div className="bean-classes">
        {SOLUTE_CLASSES.map((klass) => (
          <span key={klass} className="flavor-chip">
            {klass} <b>{(bean.classes[klass].mass_fraction * 100).toFixed(0)}%</b>
          </span>
        ))}
      </div>
      <p className="note">
        Authored priors, never validated against tasting. Changes no mass, TDS or
        yield — only the sensory estimate below the metric strip.
      </p>
    </div>
  );
}

export function ControlRail(props: Props) {
  const { recipe, onChange, issues } = props;
  const puck = (patch: Partial<Recipe["puck"]>) =>
    onChange({ ...recipe, puck: { ...recipe.puck, ...patch } });
  const stop = (patch: Partial<Recipe["stop"]>) =>
    onChange({ ...recipe, stop: { ...recipe.stop, ...patch } });

  return (
    <aside className="rail">
      <div className="section">
        <h2>Recipe</h2>
        <select
          aria-label="Recipe"
          value={props.selectedId}
          onChange={(e) => props.onSelectRecipe(e.target.value)}
        >
          {props.recipes.map((entry) =>
            "recipe" in entry ? (
              <option key={entry.id} value={entry.id}>
                {entry.name}
              </option>
            ) : (
              // Audit P4, issue #18: a malformed asset ({id, error}) is shown
              // so the catalogue explains the gap, but disabled so it can
              // never be selected and reach onSelectRecipe with no recipe.
              <option key={entry.id} value={entry.id} disabled>
                {entry.id} (failed to load: {entry.error.message})
              </option>
            ),
          )}
        </select>
      </div>

      <div className="section">
        <h2>Puck</h2>
        <NumberField
          label="Dose (g)" value={recipe.puck.dose_g} range={inputRanges.dose_g} step={0.1}
          path="recipe.puck.dose_g" issues={issues} onChange={(v) => puck({ dose_g: v })} />
        <NumberField
          label="Basket ⌀ (mm)" value={recipe.puck.basket_diameter_mm}
          range={inputRanges.basket_diameter_mm} step={0.5}
          path="recipe.puck.basket_diameter_mm" issues={issues}
          onChange={(v) => puck({ basket_diameter_mm: v })} />
        <NumberField
          label="Depth (mm)" value={recipe.puck.depth_mm} range={inputRanges.depth_mm} step={0.5}
          path="recipe.puck.depth_mm" issues={issues} onChange={(v) => puck({ depth_mm: v })} />
      </div>

      {recipe.bean && (
        <div className="section">
          <h2>Bean</h2>
          <BeanReadout bean={recipe.bean} />
        </div>
      )}

      <div className="section">
        <h2>Grind</h2>
        {recipe.puck.grind ? (
          <GrindDistributionReadout bins={recipe.puck.grind.bins} />
        ) : (
          <>
            <NumberField
              label="Particle ⌀ (µm)" value={recipe.puck.particle_diameter_um ?? 350}
              range={inputRanges.particle_diameter_um} step={5}
              path="recipe.puck.particle_diameter_um" issues={issues}
              onChange={(v) => puck({ particle_diameter_um: v })} />
            <NumberField
              label="Spread factor" value={recipe.puck.particle_spread_factor ?? 0.55}
              range={inputRanges.particle_spread_factor} step={0.05}
              path="recipe.puck.particle_spread_factor" issues={issues}
              onChange={(v) => puck({ particle_spread_factor: v })} />
            <p className="note">
              A grinder dial number has no universal physical meaning (5.3). This is an effective
              particle diameter, and the spread factor is an empirical penalty for fines.
            </p>
          </>
        )}
      </div>

      <div className="section">
        <h2>Stop conditions</h2>
        <NumberField
          label="Target mass (g)" value={recipe.stop.target_beverage_g ?? 36}
          range={inputRanges.target_beverage_g} step={1}
          path="recipe.stop.target_beverage_g" issues={issues}
          onChange={(v) => stop({ target_beverage_g: v })} />
        <NumberField
          label="Max time (s)" value={recipe.stop.maximum_time_s}
          range={inputRanges.maximum_time_s} step={1}
          path="recipe.stop.maximum_time_s" issues={issues}
          onChange={(v) => stop({ maximum_time_s: v })} />
      </div>

      <div className="section">
        <h2>Pressure profile (bar)</h2>
        <ProfileEditor
          points={recipe.pressure_profile_bar}
          range={inputRanges.pressure_bar}
          maxTimeSeconds={recipe.stop.maximum_time_s}
          unit="bar"
          color="#d98b4a"
          onChange={(points) => onChange({ ...recipe, pressure_profile_bar: points })}
        />
      </div>

      <div className="section">
        <h2>Inlet temperature (°C)</h2>
        <ProfileEditor
          points={recipe.temperature_profile_c}
          range={inputRanges.temperature_c}
          maxTimeSeconds={recipe.stop.maximum_time_s}
          unit="°C"
          color="#6fb3c8"
          onChange={(points) => onChange({ ...recipe, temperature_profile_c: points })}
        />
      </div>

      <button onClick={props.onRun} disabled={props.running || issues.length > 0}>
        {props.running ? "Simulating…" : "Run simulation"}
      </button>
      {issues.length > 0 && (
        <p className="note" style={{ marginTop: 8 }}>
          {issues.length} input{issues.length > 1 ? "s are" : " is"} outside the supported range.
        </p>
      )}
    </aside>
  );
}
