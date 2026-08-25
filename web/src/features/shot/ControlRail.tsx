import type { Recipe, ValidationIssue } from "../../api/types";
import { inputRanges } from "../../state/workspace";
import { ProfileEditor } from "./ProfileEditor";

interface Props {
  recipes: { id: string; name: string; recipe: Recipe }[];
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

// Units live in the label, never only in surrounding prose (12.5).
function NumberField({ label, value, range, step = 1, path, issues, onChange }: NumberFieldProps) {
  const invalid = issues.some((issue) => issue.path === path);
  return (
    <div className="field">
      <label title={`${range[0]} to ${range[1]}`}>{label}</label>
      <input
        type="number"
        className={invalid ? "invalid" : undefined}
        value={value}
        step={step}
        min={range[0]}
        max={range[1]}
        onChange={(event) => onChange(Number(event.target.value))}
      />
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
        <select value={props.selectedId} onChange={(e) => props.onSelectRecipe(e.target.value)}>
          {props.recipes.map((entry) => (
            <option key={entry.id} value={entry.id}>
              {entry.name}
            </option>
          ))}
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

      <div className="section">
        <h2>Grind</h2>
        <NumberField
          label="Particle ⌀ (µm)" value={recipe.puck.particle_diameter_um}
          range={inputRanges.particle_diameter_um} step={5}
          path="recipe.puck.particle_diameter_um" issues={issues}
          onChange={(v) => puck({ particle_diameter_um: v })} />
        <NumberField
          label="Spread factor" value={recipe.puck.particle_spread_factor}
          range={inputRanges.particle_spread_factor} step={0.05}
          path="recipe.puck.particle_spread_factor" issues={issues}
          onChange={(v) => puck({ particle_spread_factor: v })} />
        <p className="note">
          A grinder dial number has no universal physical meaning (5.3). This is an effective
          particle diameter, and the spread factor is an empirical penalty for fines.
        </p>
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
