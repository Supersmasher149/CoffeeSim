#pragma once
#include "espressolab/types.hpp"

namespace espressolab {

// Compressed geometry for the current pressure difference (6.3).
struct PuckGeometry {
    double depth_m = 0.0;
    double porosity = 0.0;
    double compression = 0.0;
    double pore_volume_m3 = 0.0;
};

struct FlowSolution {
    double flow_m3_s = 0.0;
    double unclamped_flow_m3_s = 0.0;
    double resistance_pa_s_m3 = 0.0;
    bool clamped_by_max_flow = false;
    bool clamped_by_backpressure = false;
};

// k0 = d_p^2 * eps^3 / (C_k * (1 - eps)^2)   (6.2)
double kozeny_carman_permeability(double particle_diameter_m, double porosity,
                                  double kozeny_constant);

// Bounded penalty for fines and a broad particle distribution (6.2).
double distribution_factor(double particle_spread_factor, double floor_value);

// Ramp from the dry multiplier to 1.0 as the puck saturates (6.4).
double wetting_factor(double saturation, double dry_multiplier);

PuckGeometry compress_puck(const Recipe& recipe, const ModelCoefficients& coeff,
                           double delta_p_pa);

// Q = (k * A / (mu * L)) * deltaP   (6.1), with the safety limits of 6.5.
FlowSolution darcy_flow(double permeability_m2, double area_m2, double viscosity_pa_s,
                        double depth_m, double delta_p_pa, double maximum_flow_m3_s);

double smoothstep(double x);

}  // namespace espressolab
