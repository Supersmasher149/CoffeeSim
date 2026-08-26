// Mirrors the result schema the native core emits (Appendix A.3). Dashboard
// units, per section 4.2: the core works in SI and converts at this boundary.

export type ProfilePoint = [number, number];

export interface ParallelRegion {
  area_fraction: number;
  permeability_multiplier: number;
}

export interface Recipe {
  schema_version: string;
  name: string;
  puck: {
    dose_g: number;
    basket_diameter_mm: number;
    depth_mm: number;
    particle_diameter_um: number;
    particle_spread_factor: number;
  };
  // Optional in the dashboard: a recipe without it is a single region, and the
  // field is carried through an edit untouched so a multi-region recipe from
  // the catalogue keeps its regions on the way back to the solver.
  parallel_regions?: ParallelRegion[];
  pressure_profile_bar: ProfilePoint[];
  temperature_profile_c: ProfilePoint[];
  stop: {
    target_beverage_g: number | null;
    maximum_time_s: number;
  };
}

export interface ShotSample {
  time_s: number;
  pressure_bar: number;
  inlet_temperature_c: number;
  puck_temperature_c: number;
  flow_ml_s: number;
  beverage_mass_g: number;
  tds_percent: number;
  extraction_yield_percent: number;
  saturation: number;
}

// Regions are reported once per run, not per sample: the solver emits a
// lateral summary, and the sample series is already region-aggregated.
export interface RegionSummary {
  area_fraction: number;
  permeability_multiplier: number;
  beverage_mass_g: number;
  flow_fraction: number;
  tds_percent: number;
  extraction_yield_percent: number;
}

export interface SimulationWarning {
  code: string;
  message: string;
  time_s: number;
  severity: "info" | "soft" | "hard";
}

export interface RunManifest {
  run_id: string;
  result_schema_version: string;
  solver_version: string;
  recipe_hash: string;
  coefficient_hash: string;
  result_hash: string;
  coefficient_id: string;
  coefficient_version: string;
  timestamp_utc: string;
  dt_s: number;
  sample_interval_s: number;
}

export interface ShotResult {
  manifest: RunManifest;
  termination: string;
  elapsed_time_s: number;
  target_mass_reached: boolean;
  beverage_mass_g: number;
  average_flow_ml_s: number;
  peak_flow_ml_s: number;
  tds_percent: number;
  extraction_yield_percent: number;
  brew_ratio: number;
  warning_count: number;
  diagnostics: {
    water_mass_residual_g: number;
    solids_mass_residual_g: number;
    clamp_count: number;
    step_count: number;
    min_permeability_m2: number;
    max_flow_ml_s: number;
    min_puck_temperature_c: number;
    max_puck_temperature_c: number;
  };
  warnings: SimulationWarning[];
  samples: ShotSample[];
  regions?: RegionSummary[];
}

export interface SweepRunRow {
  index: number;
  coordinates: number[];
  run_id: string;
  termination: string;
  shot_time_s: number;
  beverage_mass_g: number;
  tds_percent: number;
  extraction_yield_percent: number;
  warning_count: number;
}

export type SweepStatus = "queued" | "running" | "complete" | "cancelled" | "failed";

// A sweep runs in the background, so this is a status document that grows into
// a result rather than a result that is either there or not.
export interface SweepResult {
  sweep_id: string;
  status: SweepStatus;
  completed: number;
  total: number;
  elapsed_s: number;
  name?: string;
  run_count?: number;
  cancelled?: boolean;
  axes?: { parameter_path: string; values: number[] }[];
  runs?: SweepRunRow[];
  error?: { code: string; message: string };
}

export interface SweepAccepted {
  sweep_id: string;
  status: SweepStatus;
  completed: number;
  total: number;
  poll: string;
}

export interface ApiError {
  error: {
    code: string;
    message: string;
    path: string;
    details?: unknown;
  };
}

export interface ValidationIssue {
  code: string;
  message: string;
  path: string;
}
