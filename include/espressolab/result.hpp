#pragma once
#include <optional>
#include <string>
#include <vector>

#include "espressolab/flavor_result.hpp"

namespace espressolab {

enum class TerminationReason {
    target_mass_reached,
    time_limit_reached,
    numerical_failure,
    invalid_state,
    not_terminated,
};

const char* to_string(TerminationReason reason);

enum class WarningSeverity { info, soft, hard };

// Section 2, FR-08: clamps and invalid states are never silent.
struct SimulationWarning {
    std::string code;
    std::string message;
    double time_s = 0.0;
    WarningSeverity severity = WarningSeverity::soft;
};

// Appendix A.3.
struct ShotSample {
    double time_s = 0.0;
    double pressure_pa = 0.0;
    double inlet_temperature_k = 0.0;
    double puck_temperature_k = 0.0;
    double flow_m3_s = 0.0;
    double beverage_mass_kg = 0.0;
    double tds_fraction = 0.0;
    double extraction_yield_fraction = 0.0;
    double saturation = 0.0;
    double permeability_m2 = 0.0;
};

// Section 10.1, "Identity".
struct RunManifest {
    std::string run_id;
    std::string recipe_hash;
    std::string coefficient_hash;
    std::string result_hash;
    std::string solver_version;
    std::string result_schema_version;
    std::string coefficient_id;
    std::string coefficient_version;
    std::string timestamp_utc;
    double dt_s = 0.0;
    double sample_interval_s = 0.0;
};

// Section 10.1, "Diagnostics": residuals close the balances listed in 9.3.
struct ShotDiagnostics {
    double water_mass_residual_kg = 0.0;
    double solids_mass_residual_kg = 0.0;
    int clamp_count = 0;
    double min_permeability_m2 = 0.0;
    double max_flow_m3_s = 0.0;
    double min_puck_temperature_k = 0.0;
    double max_puck_temperature_k = 0.0;
    long long step_count = 0;
};

struct ShotSummary {
    TerminationReason termination = TerminationReason::not_terminated;
    double elapsed_time_s = 0.0;
    bool target_mass_reached = false;
    double beverage_mass_kg = 0.0;
    double average_flow_m3_s = 0.0;
    double peak_flow_m3_s = 0.0;
    double tds_fraction = 0.0;
    double extraction_yield_fraction = 0.0;
    double brew_ratio = 0.0;
    int warning_count = 0;
};

// Level 3: the final state of one axial cell, ordered from the screen side of
// the puck down to the basket. A Level 2 run reports one of these per region.
struct AxialCellSummary {
    double saturation = 0.0;
    double temperature_k = 0.0;
    double pore_tds_fraction = 0.0;
    double extraction_yield_fraction = 0.0;
};

struct RegionSummary {
    double area_fraction = 0.0;
    double permeability_multiplier = 1.0;
    double beverage_mass_kg = 0.0;
    double flow_fraction = 0.0;
    double tds_fraction = 0.0;
    double extraction_yield_fraction = 0.0;
    std::vector<AxialCellSummary> cells;
};

struct ShotResult {
    RunManifest manifest;
    ShotSummary summary;
    ShotDiagnostics diagnostics;
    std::vector<ShotSample> samples;
    std::vector<SimulationWarning> warnings;
    std::vector<RegionSummary> regions;
    // Engaged only when the recipe carried a bean. Additive and optional at
    // every layer: the JSON key, the CSV file and the dashboard panel all appear
    // only alongside it, so a beanless run's artifacts are byte-for-byte what
    // they were before the overlay existed.
    std::optional<FlavorResult> flavor;
};

}  // namespace espressolab
