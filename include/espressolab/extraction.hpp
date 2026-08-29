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

// The same rate at an explicit particle diameter rather than the recipe's
// representative one. Size-resolved extraction calls this once per PSD bin:
// grind_factor() is already (d_ref/d)^n, so a bin of fines simply gets the
// larger factor its surface-area-to-volume ratio earns. The recipe-taking
// overload above delegates here, so the scalar path is unchanged arithmetic.
double extraction_rate_coefficient_at(const ShotState& state, const ModelCoefficients& coeff,
                                      double flow_m3_s, double particle_diameter_m);

}  // namespace espressolab
