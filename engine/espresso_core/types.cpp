#include "espressolab/types.hpp"

#include <cmath>
#include <numbers>
#include <string>

#include "espressolab/units.hpp"
#include "espressolab/version.hpp"

namespace espressolab {

double Recipe::basket_area_m2() const {
    const double r = basket_diameter_m * 0.5;
    return std::numbers::pi * r * r;
}

// Ranges are the "initial range" column of section 5.1. They are deliberately
// hard bounds: FR-08 requires nonphysical input to fail loudly rather than
// quietly producing a plausible-looking curve.
ValidationResult Recipe::validate() const {
    ValidationResult result;

    if (schema_version != version::kRecipeSchema) {
        result.add("UNSUPPORTED_SCHEMA_VERSION",
                   "recipe schema_version '" + schema_version + "' is not supported (expected " +
                       std::string(version::kRecipeSchema) + ")",
                   "recipe.schema_version");
    }

    require_in_range(result, units::kg_to_grams(dose_kg), 14.0, 22.0, "recipe.puck.dose_g", "g");
    require_in_range(result, units::m_to_mm(basket_diameter_m), 51.0, 58.5,
                     "recipe.puck.basket_diameter_mm", "mm");
    require_in_range(result, units::m_to_mm(puck_depth_m), 6.0, 14.0, "recipe.puck.depth_mm", "mm");
    if (grind.has_value()) {
        // The scalars are derived from the distribution here, so validating them
        // as authored input would report a path the author never wrote. Check
        // the distribution instead, then confirm the derivation landed inside
        // the same envelope the scalar form is held to.
        const ValidationResult grind_result = grind->validate();
        result.merge(grind_result);
        if (grind_result.ok()) {
            const double d32_m = grind->sauter_mean_diameter_m();
            const double derived_diameter_um = units::m_to_microns(d32_m);
            if (derived_diameter_um < 150.0 || derived_diameter_um > 800.0) {
                result.add("NONPHYSICAL_INPUT",
                           "recipe.puck.grind has a Sauter mean diameter of " +
                               std::to_string(derived_diameter_um) +
                               " um, outside the supported 150-800 um range",
                           "recipe.puck.grind.bins");
            }
            // The loader keeps particle_diameter_m equal to the bins' d32, and
            // the flow path reads that scalar while extraction reads the bins.
            // A Recipe assembled in code that sets one without the other would
            // therefore run two different grinds at once -- silently, since the
            // numbers stay individually plausible. Catch it here.
            if (std::abs(particle_diameter_m - d32_m) > 1.0e-12 * std::max(d32_m, 1.0e-9)) {
                result.add("NONPHYSICAL_INPUT",
                           "recipe.puck.particle_diameter_m does not match the Sauter mean "
                           "diameter of recipe.puck.grind; it is derived from the distribution "
                           "and must not be set independently",
                           "recipe.puck.grind");
            }
        }
    } else {
        require_in_range(result, units::m_to_microns(particle_diameter_m), 150.0, 800.0,
                         "recipe.puck.particle_diameter_um", "um");
        require_in_range(result, particle_spread_factor, 0.1, 1.0,
                          "recipe.puck.particle_spread_factor", "");
    }
    if (parallel_regions.empty() || parallel_regions.size() > 8) {
        result.add("NONPHYSICAL_INPUT", "recipe.parallel_regions must contain between 1 and 8 regions",
                   "recipe.parallel_regions");
    } else {
        double total_area_fraction = 0.0;
        for (std::size_t i = 0; i < parallel_regions.size(); ++i) {
            const ParallelRegion& region = parallel_regions[i];
            const std::string path = "recipe.parallel_regions[" + std::to_string(i) + "]";
            const std::string area_path = path + ".area_fraction";
            const std::string permeability_path = path + ".permeability_multiplier";
            require_in_range(result, region.area_fraction, 0.01, 1.0, area_path.c_str(), "");
            require_in_range(result, region.permeability_multiplier, 0.05, 20.0,
                             permeability_path.c_str(), "");
            total_area_fraction += region.area_fraction;
        }
        if (std::abs(total_area_fraction - 1.0) > 1.0e-9) {
            result.add("NONPHYSICAL_INPUT", "recipe.parallel_regions area fractions must sum to 1",
                       "recipe.parallel_regions");
        }
    }
    if (axial_cells < 1 || axial_cells > 32) {
        result.add("NONPHYSICAL_INPUT", "recipe.axial_cells must be between 1 and 32",
                   "recipe.axial_cells");
    }
    require_in_range(result, maximum_time_s, 10.0, 60.0, "recipe.stop.maximum_time_s", "s");

    if (target_beverage_mass_kg.has_value()) {
        require_in_range(result, units::kg_to_grams(*target_beverage_mass_kg), 20.0, 80.0,
                         "recipe.stop.target_beverage_g", "g");
    }

    result.merge(pressure_pa.validate("recipe.pressure_profile_bar"));
    result.merge(inlet_temperature_k.validate("recipe.temperature_profile_c"));

    if (!pressure_pa.empty()) {
        require_in_range(result, units::pa_to_bar(pressure_pa.min_value()), 0.0, 12.0,
                         "recipe.pressure_profile_bar", "bar");
        require_in_range(result, units::pa_to_bar(pressure_pa.max_value()), 0.0, 12.0,
                         "recipe.pressure_profile_bar", "bar");
    }
    if (!inlet_temperature_k.empty()) {
        require_in_range(result, units::kelvin_to_celsius(inlet_temperature_k.min_value()), 85.0,
                         100.0, "recipe.temperature_profile_c", "C");
        require_in_range(result, units::kelvin_to_celsius(inlet_temperature_k.max_value()), 85.0,
                         100.0, "recipe.temperature_profile_c", "C");
    }

    return result;
}

ValidationResult ModelCoefficients::validate() const {
    ValidationResult result;

    require_in_range(result, initial_porosity, 0.05, 0.95, "coefficients.initial_porosity", "");
    require_positive(result, kozeny_constant, "coefficients.kozeny_constant");
    require_in_range(result, dry_permeability_multiplier, 0.0, 1.0,
                     "coefficients.dry_permeability_multiplier", "");
    require_in_range(result, maximum_compression, 0.0, 0.9, "coefficients.maximum_compression", "");
    require_in_range(result, pressure_compressibility, 0.0, 1.0,
                     "coefficients.pressure_compressibility", "");
    require_in_range(result, minimum_porosity, 0.01, initial_porosity,
                     "coefficients.minimum_porosity", "");
    require_positive(result, compression_reference_pa, "coefficients.compression_reference_pa");
    // Audit F5, issue #3: schemas/coefficients.schema.json documents
    // porosity_compression_factor >= 0, but nothing checked it here.
    require_nonnegative(result, porosity_compression_factor, "coefficients.porosity_compression_factor");
    require_positive(result, coffee_heat_capacity_j_kg_k,
                     "coefficients.coffee_heat_capacity_j_kg_k");
    // Was `if (ambient_heat_loss_w_k < 0.0)`: NaN compares false against
    // everything, so a non-finite value passed silently (same class of bug
    // as #7's dt_s check). require_nonnegative() rejects it first.
    require_nonnegative(result, ambient_heat_loss_w_k, "coefficients.ambient_heat_loss_w_k");
    // Kelvin temperatures below absolute zero are nonphysical regardless of
    // what range a particular calibration run intends.
    require_positive(result, ambient_temperature_k, "coefficients.ambient_temperature_k");
    require_positive(result, initial_puck_temperature_k, "coefficients.initial_puck_temperature_k");
    require_in_range(result, extractable_solids_fraction, 0.05, 0.5,
                     "coefficients.extractable_solids_fraction", "");
    require_positive(result, extraction_rate_ref_s, "coefficients.extraction_rate_ref_s");
    // Schema leaves activation_energy_j_mol's range open; only finiteness is
    // enforced here so a NaN/infinite value can't reach the Arrhenius term.
    require_finite(result, activation_energy_j_mol, "coefficients.activation_energy_j_mol");
    require_positive(result, reference_temperature_k, "coefficients.reference_temperature_k");
    require_positive(result, reference_particle_diameter_m,
                     "coefficients.reference_particle_diameter_m");
    require_positive(result, flow_half_saturation_m3_s, "coefficients.flow_half_saturation_m3_s");
    require_in_range(result, grind_exponent, 0.0, 4.0, "coefficients.grind_exponent", "");
    require_in_range(result, distribution_factor_floor, 0.01, 1.0,
                     "coefficients.distribution_factor_floor", "");
    require_positive(result, maximum_flow_m3_s, "coefficients.maximum_flow_m3_s");
    // Audit F5, issue #3: outlet_pressure_pa is the specific field the
    // finding was filed for -- omitted entirely despite the schema
    // documenting `minimum: 0`.
    require_nonnegative(result, outlet_pressure_pa, "coefficients.outlet_pressure_pa");

    return result;
}

}  // namespace espressolab
