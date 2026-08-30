import type { SweepAccepted, SweepResult, SweepRunRow } from "../../api/types";

export function makeSweepRow(overrides: Partial<SweepRunRow> = {}): SweepRunRow {
  return {
    index: 0,
    coordinates: [300],
    run_id: "run-sweep-0000",
    termination: "target_mass_reached",
    shot_time_s: 27,
    beverage_mass_g: 36,
    tds_percent: 9.4,
    extraction_yield_percent: 20,
    warning_count: 0,
    ...overrides,
  };
}

export function makeSweepAccepted(overrides: Partial<SweepAccepted> = {}): SweepAccepted {
  return {
    sweep_id: "sweep-0001",
    status: "queued",
    completed: 0,
    total: 9,
    poll: "/api/v1/sweeps/sweep-0001",
    ...overrides,
  };
}

export function makeRunningSweep(overrides: Partial<SweepResult> = {}): SweepResult {
  return {
    sweep_id: "sweep-0001",
    status: "running",
    completed: 3,
    total: 9,
    elapsed_s: 1.2,
    ...overrides,
  };
}

export function makeCompletedSweep(overrides: Partial<SweepResult> = {}): SweepResult {
  const values = [250, 300, 350];
  return {
    sweep_id: "sweep-0001",
    status: "complete",
    completed: 3,
    total: 3,
    elapsed_s: 2.4,
    name: "dashboard",
    run_count: 3,
    cancelled: false,
    axes: [{ parameter_path: "puck.particle_diameter_um", values }],
    runs: values.map((value, index) =>
      makeSweepRow({ index, coordinates: [value], run_id: `run-sweep-${index}` }),
    ),
    ...overrides,
  };
}
