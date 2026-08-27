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
  cells: AxialCellSummary[];
}

export interface AxialCellSummary {
  saturation: number;
  temperature_c: number;
  pore_tds_percent: number;
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

export interface ReferenceSource {
  author: string;
  experiment: string;
  article_url: string;
  experiment_log_url: string;
  de1_shot_file: string;
  data_quality: Record<string, string>;
}

export interface ReferenceCoffee {
  name: string;
  origin: string;
  process: string;
  varieties: string[];
  elevation_masl: string;
}

export interface ReferenceSetup {
  machine: string;
  shower_head: string;
  basket: string;
  profile: string;
  target_brew_ratio: number;
  bloom_time_s: number;
  coffee: ReferenceCoffee;
}

export interface ReferenceGrinder {
  model: string;
  burrs: string | null;
  setting: number;
  rpm: number | null;
}

export interface ReferenceObserved {
  dose_g: number;
  final_beverage_mass_g: number;
  final_shot_time_s: number | null;
  drip_g: number;
  peak_pressure_bar: number;
  tds_raw_pct: number;
  tds_filtered_pct: number;
  tds_uncertainty_pct_points: number;
  extraction_yield_raw_pct: number;
  extraction_yield_filtered_pct: number;
}

export interface ReferenceRecord {
  schema_version: string;
  id: string;
  file: string;
  source: ReferenceSource;
  setup: ReferenceSetup;
  grinder: ReferenceGrinder;
  observed: ReferenceObserved;
  timeseries_fields: string[];
  timeseries: unknown[];
  telemetry_available: false;
}

export interface ReferenceLoadError {
  file: string;
  code: string;
  message: string;
}

export interface ReferenceCatalogue {
  schema_version: string;
  telemetry_available: false;
  limitation: string;
  references: ReferenceRecord[];
  load_errors: ReferenceLoadError[];
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

export interface Cfd3dMesh {
  nx: number;
  ny: number;
  nz: number;
}

export type Cfd3dFieldName =
  | "pressure_pa"
  | "saturation"
  | "temperature_k"
  | "pore_tds_fraction"
  | "velocity_x_m_s"
  | "velocity_y_m_s"
  | "velocity_z_m_s";

export interface Cfd3dRunRequest {
  recipe: Recipe;
  coefficients?: Record<string, unknown>;
  mesh?: Cfd3dMesh;
  solver?: {
    dt_s?: number;
    sample_interval_s?: number;
    cfl_number?: number;
    pressure_tolerance?: number;
    pressure_max_iterations?: number;
    snapshot_interval_s?: number;
    snapshot_initial?: boolean;
    snapshot_final?: boolean;
  };
  material?: number | { uniform?: number; values?: number[] };
}

export type Cfd3dRunStatusName = "queued" | "running" | "complete" | "failed";

export interface Cfd3dRunAccepted {
  run_id: string;
  status: Cfd3dRunStatusName;
  poll: string;
}

export interface Cfd3dRunStatus {
  run_id: string;
  status: Cfd3dRunStatusName;
  snapshot_count: number;
  elapsed_s: number;
  result?: {
    termination: string;
    elapsed_time_s: number;
    beverage_mass_g: number;
    tds_percent: number;
    extraction_yield_percent: number;
    mesh: Cfd3dMesh;
    diagnostics: Record<string, number>;
    warnings: SimulationWarning[];
  };
  error?: { code: string; message: string };
}

export interface Cfd3dFieldSnapshot {
  run_id: string;
  snapshot_index: number;
  time_s: number;
  field: Cfd3dFieldName;
  mesh: Cfd3dMesh;
  ordering: "x-fastest, then y, then z";
  values: number[];
}

export interface ValidationIssue {
  code: string;
  message: string;
  path: string;
}
