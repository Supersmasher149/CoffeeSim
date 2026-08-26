#pragma once
#include <optional>
#include <string>
#include <vector>

#include "espressolab/profile.hpp"

namespace espressolab {

// A lateral puck partition. Regions are hydraulically parallel: they share the
// imposed pressure drop but retain their own liquid, heat, and solute state.
struct ParallelRegion {
    double area_fraction = 1.0;
    double permeability_multiplier = 1.0;
};

// Appendix A.1. All members are SI; JSON input is converted at load time.
struct Recipe {
    std::string schema_version = "1.0";
    std::string name = "unnamed";
    double dose_kg = 0.018;
    double basket_diameter_m = 0.058;
    double puck_depth_m = 0.009;
    double particle_diameter_m = 350.0e-6;
    double particle_spread_factor = 0.55;
    std::vector<ParallelRegion> parallel_regions{{}};
    PiecewiseLinearProfile pressure_pa;
    PiecewiseLinearProfile inlet_temperature_k;
    double maximum_time_s = 45.0;
    std::optional<double> target_beverage_mass_kg = 0.036;

    [[nodiscard]] ValidationResult validate() const;
    [[nodiscard]] double basket_area_m2() const;
};

// Appendix A.2. Empirical values only: every one of these is a calibration
// target, versioned separately from the recipe and the solver (10.2).
struct ModelCoefficients {
    std::string id = "default";
    std::string version = "1.0.0";

    double initial_porosity = 0.42;
    // Not the textbook Kozeny-Carman 180: a single representative particle
    // diameter cannot express a fines-filled, tamped bed, and a textbook 180 at
    // d_p = 350 um predicts a permeability near 1e-10 m^2 where espresso pucks
    // measure nearer 1e-15. These defaults must stay identical to
    // assets/coefficients/default-v1.json, which carries the full reasoning and
    // the provenance; a test in test_artifacts.cpp holds the two in agreement.
    double kozeny_constant = 4.0e6;
    double dry_permeability_multiplier = 0.25;
    double pressure_compressibility = 0.06;
    double maximum_compression = 0.20;
    double porosity_compression_factor = 0.8;
    double minimum_porosity = 0.20;
    double compression_reference_pa = 100000.0;

    double coffee_heat_capacity_j_kg_k = 1600.0;
    double ambient_heat_loss_w_k = 0.9;
    double ambient_temperature_k = 293.15;
    double initial_puck_temperature_k = 313.15;

    double extractable_solids_fraction = 0.30;
    double extraction_rate_ref_s = 0.21;
    double activation_energy_j_mol = 30000.0;
    double reference_temperature_k = 366.15;
    double grind_exponent = 1.0;
    double reference_particle_diameter_m = 350.0e-6;
    double flow_half_saturation_m3_s = 1.5e-6;

    double distribution_factor_floor = 0.05;
    double maximum_flow_m3_s = 5.0e-5;  // numerical guard only (6.5)
    // Pressure profiles are gauge pressure across the puck, so the outlet sits
    // at 0 by default. Kept configurable for a future backpressure model.
    double outlet_pressure_pa = 0.0;

    [[nodiscard]] ValidationResult validate() const;
};

// Section 4.4.
struct ShotState {
    double time_s = 0.0;
    double puck_temperature_k = 313.15;
    double permeability_m2 = 0.0;
    double liquid_saturation = 0.0;
    double remaining_extractable_solids_kg = 0.0;
    double dissolved_solids_kg = 0.0;  // dissolved and held in the pore liquid
    double beverage_mass_kg = 0.0;
    double cumulative_water_in_kg = 0.0;

    // Carried alongside the documented vector so the mass balances in 9.3 can
    // be closed without re-deriving them from the series.
    double retained_water_kg = 0.0;
    double dissolved_solids_in_cup_kg = 0.0;
};

}  // namespace espressolab
