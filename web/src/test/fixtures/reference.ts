import type { ReferenceCatalogue, ReferenceRecord } from "../../api/types";
import type { HealthResponse } from "../../api/client";

export function makeReferenceRecord(overrides: Partial<ReferenceRecord> = {}): ReferenceRecord {
  return {
    schema_version: "1.0",
    id: "real_gagne_shot_01",
    file: "real_gagne_shot_01.json",
    source: {
      author: "Jonathan Gagné",
      experiment: "shot 1",
      article_url: "https://example.com/article",
      experiment_log_url: "https://example.com/log",
      de1_shot_file: "shot_01.shot",
      data_quality: {},
    },
    setup: {
      machine: "Decent DE1+",
      shower_head: "stock",
      basket: "VST 18g",
      profile: "flat 9 bar",
      target_brew_ratio: 2,
      bloom_time_s: 5,
      coffee: {
        name: "Ethiopia Guji",
        origin: "Ethiopia",
        process: "washed",
        varieties: ["Heirloom"],
        elevation_masl: "1900-2100",
      },
    },
    grinder: { model: "Niche Zero", burrs: "conical", setting: 15, rpm: null },
    observed: {
      dose_g: 18,
      final_beverage_mass_g: 36,
      final_shot_time_s: 27,
      drip_g: 2,
      peak_pressure_bar: 9,
      tds_raw_pct: 9.8,
      tds_filtered_pct: 9.5,
      tds_uncertainty_pct_points: 0.2,
      extraction_yield_raw_pct: 21,
      extraction_yield_filtered_pct: 20.4,
    },
    timeseries_fields: [],
    timeseries: [],
    telemetry_available: false,
    ...overrides,
  };
}

export function makeReferenceCatalogue(overrides: Partial<ReferenceCatalogue> = {}): ReferenceCatalogue {
  return {
    schema_version: "1.0",
    telemetry_available: false,
    limitation: "Reported metadata only; no telemetry trace is stored.",
    references: [makeReferenceRecord()],
    load_errors: [],
    ...overrides,
  };
}

export function makeHealth(overrides: Partial<HealthResponse> = {}): HealthResponse {
  return {
    status: "ok",
    solver_version: "0.1.0-test",
    recipe_schema_version: "1.0",
    result_schema_version: "1.0",
    cfd3d_case_schema_version: "1.0",
    cfd3d_result_schema_version: "1.0",
    cfd3d_field_format: "flat",
    asset_root: "assets",
    reference_root: "espresso_real_world_refs",
    sweepable_parameters: ["puck.particle_diameter_um", "temperature_profile_c.constant"],
    ...overrides,
  };
}
