#include "espressolab/puck.hpp"

#include <algorithm>
#include <cmath>

namespace espressolab {

double smoothstep(double x) {
    const double t = std::clamp(x, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double kozeny_carman_permeability(double particle_diameter_m, double porosity,
                                  double kozeny_constant) {
    if (particle_diameter_m <= 0.0 || kozeny_constant <= 0.0) return 0.0;
    const double eps = std::clamp(porosity, 1.0e-4, 0.999);
    const double one_minus = 1.0 - eps;
    return (particle_diameter_m * particle_diameter_m * eps * eps * eps) /
           (kozeny_constant * one_minus * one_minus);
}

double distribution_factor(double particle_spread_factor, double floor_value) {
    // spread 0.1 (narrow) -> near 1.0, spread 1.0 (broad, many fines) -> heavy
    // penalty. Bounded to [floor, 1.0] per the table in 6.2.
    const double spread = std::clamp(particle_spread_factor, 0.0, 1.0);
    const double raw = 1.0 - 0.9 * spread * spread;
    return std::clamp(raw, std::clamp(floor_value, 0.01, 1.0), 1.0);
}

double wetting_factor(double saturation, double dry_multiplier) {
    const double dry = std::clamp(dry_multiplier, 0.0, 1.0);
    return dry + (1.0 - dry) * smoothstep(saturation);
}

PuckGeometry compress_puck(const Recipe& recipe, const ModelCoefficients& coeff,
                           double delta_p_pa) {
    PuckGeometry geometry;
    const double dp = std::max(delta_p_pa, 0.0);
    const double p_ref = std::max(coeff.compression_reference_pa, 1.0);

    // compression = clamp(c_p * log1p(deltaP / P_ref), 0, compression_max)  (6.3)
    geometry.compression = std::clamp(coeff.pressure_compressibility * std::log1p(dp / p_ref),
                                      0.0, coeff.maximum_compression);
    geometry.depth_m = recipe.puck_depth_m * (1.0 - geometry.compression);
    geometry.porosity = std::clamp(
        coeff.initial_porosity * (1.0 - coeff.porosity_compression_factor * geometry.compression),
        coeff.minimum_porosity, coeff.initial_porosity);
    geometry.pore_volume_m3 = recipe.basket_area_m2() * geometry.depth_m * geometry.porosity;
    return geometry;
}

FlowSolution darcy_flow(double permeability_m2, double area_m2, double viscosity_pa_s,
                        double depth_m, double delta_p_pa, double maximum_flow_m3_s) {
    FlowSolution flow;

    // 6.5: reject nonpositive viscosity, depth, area or permeability before
    // dividing, and clamp a negative pressure difference to zero flow.
    if (permeability_m2 <= 0.0 || area_m2 <= 0.0 || viscosity_pa_s <= 0.0 || depth_m <= 0.0) {
        return flow;
    }
    if (delta_p_pa <= 0.0) {
        flow.clamped_by_backpressure = delta_p_pa < 0.0;
        return flow;
    }

    const double conductance = (permeability_m2 * area_m2) / (viscosity_pa_s * depth_m);
    flow.resistance_pa_s_m3 = 1.0 / conductance;
    flow.unclamped_flow_m3_s = conductance * delta_p_pa;
    flow.flow_m3_s = flow.unclamped_flow_m3_s;

    if (maximum_flow_m3_s > 0.0 && flow.flow_m3_s > maximum_flow_m3_s) {
        flow.flow_m3_s = maximum_flow_m3_s;
        flow.clamped_by_max_flow = true;
    }
    return flow;
}

}  // namespace espressolab
