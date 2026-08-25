#pragma once
#include "espressolab/types.hpp"

namespace espressolab {

// Section 8.3. Each factor is separately testable and separately guardrailed.
double temperature_factor(double puck_temperature_k, const ModelCoefficients& coeff);
double grind_factor(double particle_diameter_m, const ModelCoefficients& coeff);
double saturation_factor(double saturation);
double flow_contact_factor(double flow_m3_s, double half_saturation_m3_s);

// k_ext = k_ref * temperature * grind * saturation * flow_contact   (8.2)
double extraction_rate_coefficient(const ShotState& state, const Recipe& recipe,
                                   const ModelCoefficients& coeff, double flow_m3_s);

}  // namespace espressolab
