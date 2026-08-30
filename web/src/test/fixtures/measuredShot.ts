import type { MeasuredShotCatalogue, MeasuredShotComparison } from "../../api/types";

export function makeMeasuredShotCatalogue(
  overrides: Partial<MeasuredShotCatalogue> = {},
): MeasuredShotCatalogue {
  return {
    schema_version: "1.0",
    measured_shots: [
      {
        id: "shot-001",
        source_stem: "shot-001",
        machine: "Decent DE1",
        date: "2026-01-01",
        notes: "",
        synthetic: true,
        final: { beverage_mass_g: 36, shot_time_s: 28, tds_percent: 9.5 },
      },
      {
        id: "shot-002",
        source_stem: "shot-002",
        machine: "Decent DE1",
        date: "2026-01-02",
        notes: "real recording",
        synthetic: false,
        final: { beverage_mass_g: null, shot_time_s: null, tds_percent: null },
      },
    ],
    count: 2,
    ...overrides,
  };
}

export function makeMeasuredShotComparison(
  overrides: Partial<MeasuredShotComparison> = {},
): MeasuredShotComparison {
  return {
    schema_version: "1.0",
    id: "shot-001",
    source_stem: "shot-001",
    machine: "Decent DE1",
    date: "2026-01-01",
    notes: "",
    synthetic: true,
    coefficients: { selector: "default-v1", id: "default", version: "v1", hash: "d".repeat(64) },
    simulation: {
      termination: "target_mass_reached",
      solver_version: "0.1.0-test",
      result_hash: "e".repeat(64),
    },
    paired_series: [
      { time_s: 0, measured_mass_g: 0, simulated_mass_g: 0, residual_g: 0 },
      { time_s: 10, measured_mass_g: 5, simulated_mass_g: 4.6, residual_g: 0.4 },
      { time_s: 20, measured_mass_g: 22, simulated_mass_g: 21.1, residual_g: 0.9 },
      { time_s: 28, measured_mass_g: 36, simulated_mass_g: 35.4, residual_g: 0.6 },
    ],
    final: {
      measured: { beverage_mass_g: 36, shot_time_s: 28, tds_percent: 9.5 },
      simulated: { beverage_mass_g: 35.4, shot_time_s: 27.6, tds_percent: 9.7 },
    },
    loss: {
      mass_rmse_g: 0.62,
      time_error_s: 0.4,
      tds_error_percent: 0.2,
      pressure_rmse_bar: 0.1,
      regularization: 0.01,
      total: 0.73,
      simulated: true,
      has_time_measurement: true,
      has_tds_measurement: true,
      has_pressure_measurement: false,
    },
    loss_weights: { mass: 1, time: 1, tds: 1, regularization: 0.1 },
    ...overrides,
  };
}
