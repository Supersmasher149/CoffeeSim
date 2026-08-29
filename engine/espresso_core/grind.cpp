#include "espressolab/grind.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "espressolab/units.hpp"

namespace espressolab {

double GrindDistribution::sauter_mean_diameter_m() const {
    // d32 = 1 / sum(w_i / d_i). Guarded rather than asserted: validate() is the
    // gate that rejects a malformed distribution, and this accessor is also
    // reached from tests and tools that build one by hand.
    if (bins.empty()) return 0.0;

    double inverse_sum = 0.0;
    double mass = 0.0;
    for (const GrindBin& bin : bins) {
        if (!(bin.diameter_m > 0.0) || !std::isfinite(bin.mass_fraction)) return 0.0;
        inverse_sum += bin.mass_fraction / bin.diameter_m;
        mass += bin.mass_fraction;
    }
    if (!(inverse_sum > 0.0) || !(mass > 0.0)) return 0.0;

    // Normalise by the mass actually present so a distribution whose fractions
    // sum to something other than 1 still yields a diameter rather than a
    // silently scaled one. validate() separately requires the sum to be 1.
    return mass / inverse_sum;
}

double GrindDistribution::geometric_std_dev() const {
    if (bins.size() < 2) return 1.0;

    double mass = 0.0;
    double mean_log = 0.0;
    for (const GrindBin& bin : bins) {
        if (!(bin.diameter_m > 0.0) || !std::isfinite(bin.mass_fraction)) return 1.0;
        mass += bin.mass_fraction;
        mean_log += bin.mass_fraction * std::log(bin.diameter_m);
    }
    if (!(mass > 0.0)) return 1.0;
    mean_log /= mass;

    double variance = 0.0;
    for (const GrindBin& bin : bins) {
        const double delta = std::log(bin.diameter_m) - mean_log;
        variance += bin.mass_fraction * delta * delta;
    }
    variance /= mass;
    if (!(variance > 0.0)) return 1.0;  // monodisperse, exactly 1.0
    return std::exp(std::sqrt(variance));
}

double GrindDistribution::equivalent_spread_factor() const {
    // sigma_g is 1.0 for a monodisperse grind and grows as the distribution
    // broadens, so ln(sigma_g) is 0 there and rises with polydispersity.
    // Normalising by ln(4) puts a typical espresso grind (sigma_g about 2.2) at
    // roughly 0.57 -- within rounding of the 0.55 that has been this project's
    // default spread since the scalar-only model, so switching a recipe over to
    // a distribution does not silently step its permeability.
    constexpr double kReferenceSigma = 4.0;
    const double sigma = geometric_std_dev();
    if (!(sigma > 1.0)) return 0.1;  // monodisperse: the narrow end of the scale
    return std::clamp(std::log(sigma) / std::log(kReferenceSigma), 0.1, 1.0);
}

ValidationResult GrindDistribution::validate() const {
    ValidationResult result;

    if (bins.size() < kMinGrindBins || bins.size() > kMaxGrindBins) {
        result.add("NONPHYSICAL_INPUT",
                   "recipe.puck.grind.bins must contain between " +
                       std::to_string(kMinGrindBins) + " and " + std::to_string(kMaxGrindBins) +
                       " bins",
                   "recipe.puck.grind.bins");
        return result;  // the per-bin checks below would only add noise
    }

    double total_mass_fraction = 0.0;
    double previous_diameter_m = 0.0;
    for (std::size_t i = 0; i < bins.size(); ++i) {
        const GrindBin& bin = bins[i];
        const std::string path = "recipe.puck.grind.bins[" + std::to_string(i) + "]";

        // Wider than the scalar envelope on purpose -- see kMinBinDiameterUm.
        // The narrow check is applied to the derived d32 in Recipe::validate().
        require_in_range(result, units::m_to_microns(bin.diameter_m), kMinBinDiameterUm,
                         kMaxBinDiameterUm, (path + ".diameter_um").c_str(), "um");
        require_in_range(result, bin.mass_fraction, 0.0, 1.0, (path + ".mass_fraction").c_str(),
                         "");

        // Strictly increasing: canonical bin order is part of the data contract,
        // so the serialized form (and therefore the recipe hash) is stable.
        if (i > 0 && !(bin.diameter_m > previous_diameter_m)) {
            result.add("NONPHYSICAL_INPUT",
                       "recipe.puck.grind.bins must be ordered by strictly increasing diameter",
                       path + ".diameter_um");
        }
        previous_diameter_m = bin.diameter_m;
        total_mass_fraction += bin.mass_fraction;
    }

    if (std::abs(total_mass_fraction - 1.0) > 1.0e-9) {
        result.add("NONPHYSICAL_INPUT", "recipe.puck.grind.bins mass fractions must sum to 1",
                   "recipe.puck.grind.bins");
    }
    return result;
}

}  // namespace espressolab
