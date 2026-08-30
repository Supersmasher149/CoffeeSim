import type { Recipe, ShotResult, ValidationIssue } from "../api/types";

// Section 12.4.
export interface ShotWorkspace {
  draftRecipe: Recipe;
  validation: ValidationIssue[];
  activeRun?: ShotResult;
  // Audit P7, issue #22: the exact recipe submitted to produce activeRun,
  // captured at request time and left untouched by later draftRecipe edits.
  // ShotResult carries no recipe of its own (just hashes), so result
  // presentation that depends on recipe fields -- the puck view's target
  // mass, the chart's pre-infusion marker -- must read this, not
  // draftRecipe, or editing the draft after a run silently relabels what
  // the result actually shows.
  activeRecipe?: Recipe;
  comparisonRunIds: string[];
  cursorTimeSeconds?: number;
  requestState: "idle" | "running" | "failed";
}

// Used only until the server's recipe list arrives, so the controls always have
// something coherent to render.
export const fallbackRecipe: Recipe = {
  schema_version: "1.0",
  name: "Baseline 18 g espresso",
  puck: {
    dose_g: 18,
    basket_diameter_mm: 58,
    depth_mm: 9,
    particle_diameter_um: 350,
    particle_spread_factor: 0.55,
  },
  pressure_profile_bar: [
    [0, 2],
    [6, 2],
    [10, 9],
    [30, 9],
  ],
  temperature_profile_c: [[0, 93]],
  stop: { target_beverage_g: 36, maximum_time_s: 45 },
};

// The dashboard never calculates anything authoritative (3.1), but it can
// mirror the core's input ranges (5.1) to disable a Run button before a
// round trip that is certain to fail.
export const inputRanges = {
  dose_g: [14, 22],
  basket_diameter_mm: [51, 58.5],
  depth_mm: [6, 14],
  particle_diameter_um: [150, 800],
  particle_spread_factor: [0.1, 1],
  maximum_time_s: [10, 60],
  target_beverage_g: [20, 80],
  pressure_bar: [0, 12],
  temperature_c: [85, 100],
} as const;

export function localValidation(recipe: Recipe): ValidationIssue[] {
  const issues: ValidationIssue[] = [];
  const check = (value: number, path: string, [low, high]: readonly [number, number]) => {
    if (!Number.isFinite(value) || value < low || value > high) {
      issues.push({
        code: "OUT_OF_RANGE",
        path,
        message: `${path} must be between ${low} and ${high} (currently ${value})`,
      });
    }
  };

  check(recipe.puck.dose_g, "recipe.puck.dose_g", inputRanges.dose_g);
  check(recipe.puck.basket_diameter_mm, "recipe.puck.basket_diameter_mm", inputRanges.basket_diameter_mm);
  check(recipe.puck.depth_mm, "recipe.puck.depth_mm", inputRanges.depth_mm);
  // The scalar pair and the distribution are mutually exclusive spellings of
  // the same input, so only the one actually present is range-checked. A PSD's
  // own constraints (ordering, mass fractions, the derived d32 envelope) are
  // the native validator's to rule on -- section 4.2's "no browser-side
  // authoritative calculation" applies to derived quantities like d32.
  if (recipe.puck.grind === undefined) {
    check(recipe.puck.particle_diameter_um ?? NaN, "recipe.puck.particle_diameter_um", inputRanges.particle_diameter_um);
    check(recipe.puck.particle_spread_factor ?? NaN, "recipe.puck.particle_spread_factor", inputRanges.particle_spread_factor);
  }
  check(recipe.stop.maximum_time_s, "recipe.stop.maximum_time_s", inputRanges.maximum_time_s);
  if (recipe.stop.target_beverage_g !== null) {
    check(recipe.stop.target_beverage_g, "recipe.stop.target_beverage_g", inputRanges.target_beverage_g);
  }

  for (const [index, [time, value]] of recipe.pressure_profile_bar.entries()) {
    check(value, `recipe.pressure_profile_bar[${index}]`, inputRanges.pressure_bar);
    if (index > 0 && time <= recipe.pressure_profile_bar[index - 1][0]) {
      issues.push({
        code: "UNORDERED_PROFILE",
        path: `recipe.pressure_profile_bar[${index}]`,
        message: "profile times must strictly increase",
      });
    }
  }
  for (const [index, [time, value]] of recipe.temperature_profile_c.entries()) {
    check(value, `recipe.temperature_profile_c[${index}]`, inputRanges.temperature_c);
    if (index > 0 && time <= recipe.temperature_profile_c[index - 1][0]) {
      issues.push({
        code: "UNORDERED_PROFILE",
        path: `recipe.temperature_profile_c[${index}]`,
        message: "profile times must strictly increase",
      });
    }
  }
  return issues;
}

// The end of pre-infusion is the first time the commanded pressure reaches its
// maximum, which is what the chart marks (12.5).
export function preInfusionEnd(recipe: Recipe): number | undefined {
  const points = recipe.pressure_profile_bar;
  if (points.length < 2) return undefined;
  const peak = Math.max(...points.map(([, bar]) => bar));
  const first = points.find(([, bar]) => bar >= peak - 1e-9);
  if (!first || first[0] <= 0) return undefined;
  return first[0];
}
