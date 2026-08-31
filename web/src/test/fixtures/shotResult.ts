import type { ShotResult, ShotSample } from "../../api/types";

function sample(overrides: Partial<ShotSample>): ShotSample {
  return {
    time_s: 0,
    pressure_bar: 2,
    inlet_temperature_c: 93,
    puck_temperature_c: 22,
    flow_ml_s: 0,
    beverage_mass_g: 0,
    tds_percent: 0,
    extraction_yield_percent: 0,
    saturation: 0,
    ...overrides,
  };
}

export function makeSamples(): ShotSample[] {
  return [
    sample({ time_s: 0, saturation: 0, flow_ml_s: 0.5, puck_temperature_c: 22 }),
    sample({ time_s: 5, saturation: 0.6, flow_ml_s: 1.2, puck_temperature_c: 60, pressure_bar: 2 }),
    sample({
      time_s: 10,
      saturation: 1,
      flow_ml_s: 2.4,
      puck_temperature_c: 88,
      pressure_bar: 9,
      beverage_mass_g: 4,
      tds_percent: 8,
      extraction_yield_percent: 10,
    }),
    sample({
      time_s: 20,
      saturation: 1,
      flow_ml_s: 2.1,
      puck_temperature_c: 91,
      pressure_bar: 9,
      beverage_mass_g: 20,
      tds_percent: 9.5,
      extraction_yield_percent: 18,
    }),
    sample({
      time_s: 28,
      saturation: 1,
      flow_ml_s: 1.9,
      puck_temperature_c: 92,
      pressure_bar: 9,
      beverage_mass_g: 36,
      tds_percent: 9.8,
      extraction_yield_percent: 21,
    }),
  ];
}

let runIdSequence = 0;

export function makeShotResult(overrides: Partial<ShotResult> = {}): ShotResult {
  runIdSequence += 1;
  return {
    manifest: {
      run_id: `run-${String(runIdSequence).padStart(4, "0")}`,
      result_schema_version: "1.0",
      solver_version: "0.1.0-test",
      recipe_hash: "a".repeat(64),
      coefficient_hash: "b".repeat(64),
      result_hash: "c".repeat(64),
      coefficient_id: "default",
      coefficient_version: "v1",
      timestamp_utc: "2026-08-01T00:00:00Z",
      dt_s: 0.02,
      sample_interval_s: 0.5,
    },
    termination: "target_mass_reached",
    elapsed_time_s: 28,
    target_mass_reached: true,
    beverage_mass_g: 36,
    average_flow_ml_s: 1.9,
    peak_flow_ml_s: 2.4,
    tds_percent: 9.8,
    extraction_yield_percent: 21,
    brew_ratio: 2,
    warning_count: 0,
    diagnostics: {
      water_mass_residual_g: 1e-9,
      solids_mass_residual_g: 1e-10,
      clamp_count: 0,
      step_count: 1400,
      min_permeability_m2: 1e-13,
      max_flow_ml_s: 2.4,
      min_puck_temperature_c: 22,
      max_puck_temperature_c: 92,
    },
    warnings: [],
    samples: makeSamples(),
    ...overrides,
  };
}

export function resetRunIdSequence() {
  runIdSequence = 0;
}
