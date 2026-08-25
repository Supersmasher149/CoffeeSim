#include "espressolab/extraction.hpp"

#include <algorithm>
#include <cmath>

#include "espressolab/puck.hpp"
#include "espressolab/units.hpp"

namespace espressolab {

double temperature_factor(double puck_temperature_k, const ModelCoefficients& coeff) {
    if (puck_temperature_k <= 0.0) return 0.0;
    const double t_ref = std::max(coeff.reference_temperature_k, 1.0);
    const double exponent = -(coeff.activation_energy_j_mol / units::kGasConstantJMolK) *
                            (1.0 / puck_temperature_k - 1.0 / t_ref);
    // Clamp the exponent to a safe range (8.3) so a bad temperature cannot
    // produce inf and poison every downstream state value.
    return std::exp(std::clamp(exponent, -10.0, 10.0));
}

double grind_factor(double particle_diameter_m, const ModelCoefficients& coeff) {
    if (particle_diameter_m <= 0.0) return 0.0;
    const double d_ref = std::max(coeff.reference_particle_diameter_m, 1.0e-9);
    const double raw = std::pow(d_ref / particle_diameter_m, coeff.grind_exponent);
    return std::clamp(raw, 0.05, 20.0);
}

double saturation_factor(double saturation) {
    // Exactly zero at a dry puck (8.3).
    if (saturation <= 0.0) return 0.0;
    return smoothstep(saturation);
}

double flow_contact_factor(double flow_m3_s, double half_saturation_m3_s) {
    const double q = std::max(flow_m3_s, 0.0);
    const double q_half = std::max(half_saturation_m3_s, 1.0e-12);
    return q / (q + q_half);  // bounded below 1, no unbounded growth (8.3)
}

double extraction_rate_coefficient(const ShotState& state, const Recipe& recipe,
                                   const ModelCoefficients& coeff, double flow_m3_s) {
    return coeff.extraction_rate_ref_s * temperature_factor(state.puck_temperature_k, coeff) *
           grind_factor(recipe.particle_diameter_m, coeff) *
           saturation_factor(state.liquid_saturation) *
           flow_contact_factor(flow_m3_s, coeff.flow_half_saturation_m3_s);
}

}  // namespace espressolab
