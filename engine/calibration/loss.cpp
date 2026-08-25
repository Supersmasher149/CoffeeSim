#include <algorithm>
#include <cmath>
#include <set>

#include "espressolab/calibration.hpp"
#include "espressolab/units.hpp"

namespace espressolab::calibration {
namespace {

double interpolate_pressure_bar(const std::vector<ShotSample>& samples, double time_s) {
    if (samples.empty()) return 0.0;
    if (time_s <= samples.front().time_s) return units::pa_to_bar(samples.front().pressure_pa);
    if (time_s >= samples.back().time_s) return units::pa_to_bar(samples.back().pressure_pa);

    const auto upper = std::lower_bound(
        samples.begin(), samples.end(), time_s,
        [](const ShotSample& sample, double t) { return sample.time_s < t; });
    const auto lower = upper - 1;
    const double span = upper->time_s - lower->time_s;
    const double f = span > 0.0 ? (time_s - lower->time_s) / span : 0.0;
    return units::pa_to_bar(lower->pressure_pa + f * (upper->pressure_pa - lower->pressure_pa));
}

}  // namespace

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
        if (series[i].pressure_bar.has_value() &&
            !std::isfinite(*series[i].pressure_bar)) {
            result.add("NONFINITE_INPUT", path + " pressure must be finite when supplied", path);
        }
        previous_time = series[i].time_s;
    }
    if (final_beverage_mass_g.has_value() &&
        (!std::isfinite(*final_beverage_mass_g) || *final_beverage_mass_g < 0.0)) {
        result.add("NONPHYSICAL_INPUT", "final beverage mass must be finite and nonnegative",
                   "measured_shot.final.beverage_mass_g");
    }
    if (final_shot_time_s.has_value() &&
        (!std::isfinite(*final_shot_time_s) || *final_shot_time_s < 0.0)) {
        result.add("NONPHYSICAL_INPUT", "final shot time must be finite and nonnegative",
                   "measured_shot.final.shot_time_s");
    }
    if (final_tds_percent.has_value() &&
        (!std::isfinite(*final_tds_percent) || *final_tds_percent < 0.0)) {
        result.add("NONPHYSICAL_INPUT", "final TDS must be finite and nonnegative",
                   "measured_shot.final.tds_percent");
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
        breakdown.has_time_measurement = true;
        breakdown.time_error_s = std::abs(result.summary.elapsed_time_s - *shot.final_shot_time_s);
    }
    if (shot.final_tds_percent.has_value()) {
        breakdown.has_tds_measurement = true;
        breakdown.tds_error_percent =
            std::abs(result.summary.tds_fraction * 100.0 - *shot.final_tds_percent);
    }
    double pressure_sum_squared = 0.0;
    std::size_t pressure_count = 0;
    for (const auto& sample : shot.series) {
        if (!sample.pressure_bar.has_value()) continue;
        const double error = interpolate_pressure_bar(result.samples, sample.time_s) -
                             *sample.pressure_bar;
        pressure_sum_squared += error * error;
        ++pressure_count;
    }
    if (pressure_count > 0) {
        breakdown.has_pressure_measurement = true;
        breakdown.pressure_rmse_bar =
            std::sqrt(pressure_sum_squared / static_cast<double>(pressure_count));
    }

    breakdown.total = weights.mass * breakdown.mass_rmse_g + weights.time * breakdown.time_error_s +
                      weights.tds * breakdown.tds_error_percent;
    return breakdown;
}

ValidationResult validate_leave_one_out_dataset(const std::vector<MeasuredShot>& shots) {
    ValidationResult result;
    if (shots.size() < 3) {
        result.add("INSUFFICIENT_SHOTS", "leave-one-out validation requires at least three shots",
                   "calibration.shots");
    }

    std::set<std::string> ids;
    std::string machine;
    for (std::size_t i = 0; i < shots.size(); ++i) {
        const MeasuredShot& shot = shots[i];
        const std::string path = "calibration.shots[" + std::to_string(i) + "]";
        result.merge(shot.validate());
        if (shot.synthetic) {
            result.add("SYNTHETIC_DATA", "leave-one-out validation requires real measured shots", path);
        }
        if (shot.id.empty() || !ids.insert(shot.id).second) {
            result.add("DUPLICATE_SHOT_ID", "each leave-one-out shot needs a unique nonempty id",
                       path + ".id");
        }
        if (shot.machine.empty()) {
            result.add("MISSING_MACHINE", "leave-one-out validation requires a machine description",
                       path + ".machine");
        } else if (machine.empty()) {
            machine = shot.machine;
        } else if (shot.machine != machine) {
            result.add("MIXED_MACHINE", "all leave-one-out shots must use the same machine setup",
                       path + ".machine");
        }
        if (shot.series.size() < 2) {
            result.add("INSUFFICIENT_MASS_SERIES",
                       "leave-one-out validation requires at least two time/mass samples per shot",
                       path + ".series");
        }
        if (!shot.final_shot_time_s.has_value() || *shot.final_shot_time_s <= 0.0) {
            result.add("MISSING_SHOT_TIME",
                       "leave-one-out validation requires a positive measured final shot time",
                       path + ".final");
        }
    }
    return result;
}

}  // namespace espressolab::calibration
