#include <algorithm>
#include <cmath>

#include "espressolab/calibration.hpp"
#include "espressolab/units.hpp"

namespace espressolab::calibration {

double interpolate_beverage_mass_g(const std::vector<ShotSample>& samples, double time_s) {
    if (samples.empty()) return 0.0;
    if (time_s <= samples.front().time_s) {
        return units::kg_to_grams(samples.front().beverage_mass_kg);
    }
    // Past the end of a shot that stopped early, the cup stops filling. Holding
    // the final value is what makes a too-fast candidate pay for the mass it
    // never delivered.
    if (time_s >= samples.back().time_s) {
        return units::kg_to_grams(samples.back().beverage_mass_kg);
    }

    const auto upper = std::lower_bound(
        samples.begin(), samples.end(), time_s,
        [](const ShotSample& sample, double t) { return sample.time_s < t; });
    const auto lower = upper - 1;
    const double span = upper->time_s - lower->time_s;
    const double f = span > 0.0 ? (time_s - lower->time_s) / span : 0.0;
    return units::kg_to_grams(lower->beverage_mass_kg +
                              f * (upper->beverage_mass_kg - lower->beverage_mass_kg));
}

ValidationResult MeasuredShot::validate() const {
    ValidationResult result;
    if (series.empty() && !final_beverage_mass_g.has_value()) {
        result.add("EMPTY_MEASUREMENT",
                   "a measured shot needs either a mass series or a final beverage mass",
                   "measured_shot.series");
    }
    double previous_time = -1.0;
    for (std::size_t i = 0; i < series.size(); ++i) {
        const std::string path = "measured_shot.series[" + std::to_string(i) + "]";
        if (!std::isfinite(series[i].time_s) || !std::isfinite(series[i].beverage_mass_g)) {
            result.add("NONFINITE_INPUT", path + " must contain finite numbers", path);
            continue;
        }
        if (series[i].time_s <= previous_time) {
            result.add("UNORDERED_SERIES", path + " time must strictly increase", path);
        }
        if (series[i].beverage_mass_g < 0.0) {
            result.add("NONPHYSICAL_INPUT", path + " beverage mass must not be negative", path);
        }
        previous_time = series[i].time_s;
    }
    result.merge(recipe.validate());
    return result;
}

LossBreakdown evaluate_shot_loss(const MeasuredShot& shot, const ModelCoefficients& coefficients,
                                 const SimulationConfig& config, const LossWeights& weights) {
    LossBreakdown breakdown;

    ShotResult result;
    try {
        result = Simulator().run(shot.recipe, coefficients, config);
    } catch (const InvalidInputError&) {
        // A candidate outside the coefficient validation ranges is not a crash,
        // it is simply a very bad candidate. Give the optimiser a finite, large
        // number to walk away from rather than an exception.
        breakdown.total = 1.0e9;
        return breakdown;
    }
    breakdown.simulated = true;

    // Mass curve RMSE over the measured sample times (11.4).
    if (!shot.series.empty()) {
        double sum_squared = 0.0;
        for (const auto& sample : shot.series) {
            const double simulated = interpolate_beverage_mass_g(result.samples, sample.time_s);
            const double error = simulated - sample.beverage_mass_g;
            sum_squared += error * error;
        }
        breakdown.mass_rmse_g = std::sqrt(sum_squared / static_cast<double>(shot.series.size()));
    } else if (shot.final_beverage_mass_g.has_value()) {
        breakdown.mass_rmse_g =
            std::abs(units::kg_to_grams(result.summary.beverage_mass_kg) - *shot.final_beverage_mass_g);
    }

    if (shot.final_shot_time_s.has_value()) {
        breakdown.time_error_s = std::abs(result.summary.elapsed_time_s - *shot.final_shot_time_s);
    }
    if (shot.final_tds_percent.has_value()) {
        breakdown.tds_error_percent =
            std::abs(result.summary.tds_fraction * 100.0 - *shot.final_tds_percent);
    }

    breakdown.total = weights.mass * breakdown.mass_rmse_g + weights.time * breakdown.time_error_s +
                      weights.tds * breakdown.tds_error_percent;
    return breakdown;
}

}  // namespace espressolab::calibration
