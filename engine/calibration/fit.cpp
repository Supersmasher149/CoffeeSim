#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>

#include "espressolab/calibration.hpp"

namespace espressolab::calibration {
namespace {

// The optimiser works in a normalised box so that a Kozeny constant near 1e6 and
// a porosity near 0.4 take comparable step sizes.
double to_normalised(const TunableParameter& parameter, double value) {
    if (parameter.logarithmic) {
        const double low = std::log(std::max(parameter.low, 1.0e-300));
        const double high = std::log(std::max(parameter.high, 1.0e-300));
        return (std::log(std::max(value, 1.0e-300)) - low) / (high - low);
    }
    return (value - parameter.low) / (parameter.high - parameter.low);
}

double from_normalised(const TunableParameter& parameter, double t) {
    const double clamped = std::clamp(t, 0.0, 1.0);
    if (parameter.logarithmic) {
        const double low = std::log(std::max(parameter.low, 1.0e-300));
        const double high = std::log(std::max(parameter.high, 1.0e-300));
        return std::exp(low + clamped * (high - low));
    }
    return parameter.low + clamped * (parameter.high - parameter.low);
}

ModelCoefficients apply(const ModelCoefficients& base,
                        const std::vector<TunableParameter>& parameters,
                        const std::vector<double>& normalised) {
    ModelCoefficients candidate = base;
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        candidate = with_parameter(candidate, parameters[i].name,
                                   from_normalised(parameters[i], normalised[i]));
    }
    return candidate;
}

// The "regularization_for_nonphysical_coefficients" term of 11.4: the simplex is
// free to reflect outside the box, it just pays for it and gets pulled back.
double out_of_bounds_penalty(const std::vector<double>& normalised, double weight) {
    double penalty = 0.0;
    for (const double t : normalised) {
        if (t < 0.0) penalty += -t;
        if (t > 1.0) penalty += t - 1.0;
    }
    return penalty * weight;
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    if (values.size() % 2 != 0) return values[middle];
    return (values[middle - 1] + values[middle]) / 2.0;
}

}  // namespace

CalibrationReport fit(const CalibrationSpec& spec) {
    ValidationResult validation;
    validation.merge(spec.starting_point.validate());
    if (spec.parameters.empty()) {
        validation.add("NO_PARAMETERS", "a fit needs at least one tunable parameter",
                       "calibration.parameters");
    }
    if (spec.fitting_shots.empty()) {
        validation.add("NO_FITTING_SHOTS", "a fit needs at least one measured shot",
                       "calibration.fitting_shots");
    }
    for (const auto& shot : spec.fitting_shots) validation.merge(shot.validate());
    for (const auto& shot : spec.validation_shots) validation.merge(shot.validate());
    std::set<std::string> parameter_names;
    for (std::size_t i = 0; i < spec.parameters.size(); ++i) {
        const TunableParameter& parameter = spec.parameters[i];
        const std::string path = "calibration.parameters[" + std::to_string(i) + "]";
        if (!tunable_parameter(parameter.name).has_value()) {
            validation.add("UNKNOWN_PARAMETER_NAME",
                           "no fittable coefficient named '" + parameter.name + "'", path + ".name");
        }
        if (!parameter_names.insert(parameter.name).second) {
            validation.add("DUPLICATE_PARAMETER", "each coefficient may be fitted only once", path + ".name");
        }
        if (!std::isfinite(parameter.low) || !std::isfinite(parameter.high) ||
            parameter.low >= parameter.high || (parameter.logarithmic && parameter.low <= 0.0)) {
            validation.add("INVALID_PARAMETER_BOUNDS",
                           "parameter bounds must be finite, increasing, and positive in log space", path);
        }
    }
    const auto validate_weight = [&](double weight, const char* name) {
        if (!std::isfinite(weight) || weight < 0.0) {
            validation.add("INVALID_LOSS_WEIGHT", std::string(name) + " must be finite and nonnegative",
                           std::string("calibration.weights.") + name);
        }
    };
    validate_weight(spec.weights.mass, "mass");
    validate_weight(spec.weights.time, "time");
    validate_weight(spec.weights.tds, "tds");
    validate_weight(spec.weights.regularization, "regularization");
    if (spec.config.dt_s <= 0.0 || !std::isfinite(spec.config.dt_s) ||
        spec.config.sample_interval_s <= 0.0 || !std::isfinite(spec.config.sample_interval_s)) {
        validation.add("NONPHYSICAL_INPUT", "solver dt_s and sample_interval_s must be positive finite values",
                       "calibration.config");
    }
    if (spec.maximum_iterations <= 0) {
        validation.add("INVALID_ITERATION_COUNT", "maximum_iterations must be positive",
                       "calibration.maximum_iterations");
    }
    if (!std::isfinite(spec.tolerance) || spec.tolerance < 0.0) {
        validation.add("INVALID_TOLERANCE", "tolerance must be finite and nonnegative",
                       "calibration.tolerance");
    }
    if (!validation.ok()) throw InvalidInputError(validation);

    CalibrationReport report;
    report.parameters = spec.parameters;
    for (const auto& shot : spec.fitting_shots) {
        report.used_synthetic_data = report.used_synthetic_data || shot.synthetic;
    }
    for (const auto& shot : spec.validation_shots) {
        report.used_synthetic_data = report.used_synthetic_data || shot.synthetic;
    }

    const std::size_t n = spec.parameters.size();
    int simulations = 0;

    const auto objective = [&](const std::vector<double>& normalised) {
        const ModelCoefficients candidate = apply(spec.starting_point, spec.parameters, normalised);
        double total = 0.0;
        for (const auto& shot : spec.fitting_shots) {
            total += evaluate_shot_loss(shot, candidate, spec.config, spec.weights).total;
            ++simulations;
        }
        // Minimise the mean across shots rather than matching one exactly (11.3).
        return total / static_cast<double>(spec.fitting_shots.size()) +
               out_of_bounds_penalty(normalised, spec.weights.regularization);
    };

    std::vector<double> start(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        start[i] = std::clamp(
            to_normalised(spec.parameters[i], read_parameter(spec.starting_point, spec.parameters[i].name)),
            0.0, 1.0);
    }
    report.starting_values.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        report.starting_values.push_back(from_normalised(spec.parameters[i], start[i]));
    }
    report.starting_loss = objective(start);

    // Deterministic initial simplex: one vertex per parameter, stepped away from
    // whichever bound it is nearer (11.2 determinism carries into calibration).
    constexpr double kStep = 0.12;
    std::vector<std::vector<double>> simplex;
    std::vector<double> values;
    simplex.push_back(start);
    values.push_back(report.starting_loss);
    for (std::size_t i = 0; i < n; ++i) {
        std::vector<double> vertex = start;
        vertex[i] += (vertex[i] > 0.5) ? -kStep : kStep;
        simplex.push_back(vertex);
        values.push_back(objective(vertex));
    }

    constexpr double kReflection = 1.0;
    constexpr double kExpansion = 2.0;
    constexpr double kContraction = 0.5;
    constexpr double kShrink = 0.5;

    std::vector<std::size_t> order(simplex.size());
    int iteration = 0;
    for (; iteration < spec.maximum_iterations; ++iteration) {
        std::iota(order.begin(), order.end(), 0);
        // Stable ordering, with the vertex index breaking ties, so an equal-loss
        // simplex evolves identically on every machine.
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            if (values[a] != values[b]) return values[a] < values[b];
            return a < b;
        });

        std::vector<std::vector<double>> sorted_simplex;
        std::vector<double> sorted_values;
        sorted_simplex.reserve(order.size());
        sorted_values.reserve(order.size());
        for (const std::size_t index : order) {
            sorted_simplex.push_back(simplex[index]);
            sorted_values.push_back(values[index]);
        }
        simplex.swap(sorted_simplex);
        values.swap(sorted_values);

        if (std::abs(values.back() - values.front()) <= spec.tolerance) {
            report.converged = true;
            break;
        }

        std::vector<double> centroid(n, 0.0);
        for (std::size_t v = 0; v + 1 < simplex.size(); ++v) {
            for (std::size_t i = 0; i < n; ++i) centroid[i] += simplex[v][i];
        }
        for (double& value : centroid) value /= static_cast<double>(n);

        const auto combine = [&](double factor) {
            std::vector<double> point(n, 0.0);
            for (std::size_t i = 0; i < n; ++i) {
                point[i] = centroid[i] + factor * (centroid[i] - simplex.back()[i]);
            }
            return point;
        };

        const std::vector<double> reflected = combine(kReflection);
        const double reflected_value = objective(reflected);

        if (reflected_value < values.front()) {
            const std::vector<double> expanded = combine(kExpansion);
            const double expanded_value = objective(expanded);
            if (expanded_value < reflected_value) {
                simplex.back() = expanded;
                values.back() = expanded_value;
            } else {
                simplex.back() = reflected;
                values.back() = reflected_value;
            }
        } else if (reflected_value < values[values.size() - 2]) {
            simplex.back() = reflected;
            values.back() = reflected_value;
        } else {
            const bool use_outside_contraction = reflected_value < values.back();
            const std::vector<double> contracted =
                combine(use_outside_contraction ? kContraction : -kContraction);
            const double contracted_value = objective(contracted);
            if (contracted_value < (use_outside_contraction ? reflected_value : values.back())) {
                simplex.back() = contracted;
                values.back() = contracted_value;
            } else {
                for (std::size_t v = 1; v < simplex.size(); ++v) {
                    for (std::size_t i = 0; i < n; ++i) {
                        simplex[v][i] =
                            simplex.front()[i] + kShrink * (simplex[v][i] - simplex.front()[i]);
                    }
                    values[v] = objective(simplex[v]);
                }
            }
        }
    }

    const auto best = static_cast<std::size_t>(
        std::min_element(values.begin(), values.end()) - values.begin());
    const std::vector<double>& solution = simplex[best];

    report.iterations = iteration;
    report.simulations = simulations;
    report.fitted = apply(spec.starting_point, spec.parameters, solution);
    report.final_loss = values[best];
    for (std::size_t i = 0; i < n; ++i) {
        report.fitted_values.push_back(from_normalised(spec.parameters[i], solution[i]));
    }

    for (const auto& shot : spec.fitting_shots) {
        report.fitting_losses.push_back(
            {shot.id, evaluate_shot_loss(shot, report.fitted, spec.config, spec.weights)});
    }
    // Held-out shots are scored once, at the end, and never steer the fit.
    for (const auto& shot : spec.validation_shots) {
        const LossBreakdown loss =
            evaluate_shot_loss(shot, report.fitted, spec.config, spec.weights);
        report.validation_losses.push_back({shot.id, loss});
        report.validation_loss += loss.total;
    }
    if (!report.validation_losses.empty()) {
        report.validation_loss /= static_cast<double>(report.validation_losses.size());
    }
    return report;
}

LeaveOneOutReport leave_one_out(const CalibrationSpec& spec,
                                const LeaveOneOutThresholds& thresholds) {
    ValidationResult validation = validate_leave_one_out_dataset(spec.fitting_shots);
    if (!spec.validation_shots.empty()) {
        validation.add("UNEXPECTED_HOLDOUT_SHOTS",
                       "leave-one-out uses every shot as a validation case; do not set validation_shots",
                       "calibration.validation_shots");
    }
    const auto validate_threshold = [&](double value, const char* name) {
        if (!std::isfinite(value) || value <= 0.0) {
            validation.add("INVALID_VALIDATION_THRESHOLD",
                           std::string(name) + " must be finite and positive",
                           std::string("calibration.thresholds.") + name);
        }
    };
    validate_threshold(thresholds.median_mass_rmse_g, "median_mass_rmse_g");
    validate_threshold(thresholds.median_time_error_s, "median_time_error_s");
    validate_threshold(thresholds.median_tds_error_percent, "median_tds_error_percent");
    validate_threshold(thresholds.worst_fold_multiplier, "worst_fold_multiplier");
    if (!validation.ok()) throw InvalidInputError(validation);

    LeaveOneOutReport report;
    report.thresholds = thresholds;
    report.folds.reserve(spec.fitting_shots.size());

    for (std::size_t held_out = 0; held_out < spec.fitting_shots.size(); ++held_out) {
        CalibrationSpec fold = spec;
        fold.fitting_shots.clear();
        for (std::size_t i = 0; i < spec.fitting_shots.size(); ++i) {
            if (i != held_out) fold.fitting_shots.push_back(spec.fitting_shots[i]);
        }
        fold.validation_shots.clear();

        const CalibrationReport fitted = fit(fold);
        const MeasuredShot& shot = spec.fitting_shots[held_out];
        report.folds.push_back(
            {shot.id, evaluate_shot_loss(shot, fitted.fitted, spec.config, spec.weights)});
    }

    std::vector<double> mass_errors;
    std::vector<double> time_errors;
    std::vector<double> tds_errors;
    std::vector<double> pressure_errors;
    mass_errors.reserve(report.folds.size());
    time_errors.reserve(report.folds.size());
    for (const ShotLoss& fold : report.folds) {
        mass_errors.push_back(fold.loss.mass_rmse_g);
        if (fold.loss.has_time_measurement) time_errors.push_back(fold.loss.time_error_s);
        if (fold.loss.has_tds_measurement) tds_errors.push_back(fold.loss.tds_error_percent);
        if (fold.loss.has_pressure_measurement) pressure_errors.push_back(fold.loss.pressure_rmse_bar);
    }

    report.median_mass_rmse_g = median(mass_errors);
    report.worst_mass_rmse_g = *std::max_element(mass_errors.begin(), mass_errors.end());
    if (time_errors.size() == report.folds.size()) {
        report.median_time_error_s = median(time_errors);
        report.worst_time_error_s = *std::max_element(time_errors.begin(), time_errors.end());
    } else {
        report.failed_checks.emplace_back("shot time was not measured for every fold");
    }
    report.tds_assessed = tds_errors.size() == report.folds.size();
    if (report.tds_assessed) {
        report.median_tds_error_percent = median(tds_errors);
        report.worst_tds_error_percent = *std::max_element(tds_errors.begin(), tds_errors.end());
    }
    if (!pressure_errors.empty()) {
        report.median_pressure_rmse_bar = median(pressure_errors);
        report.worst_pressure_rmse_bar =
            *std::max_element(pressure_errors.begin(), pressure_errors.end());
    }

    if (report.median_mass_rmse_g > thresholds.median_mass_rmse_g) {
        report.failed_checks.emplace_back("median mass RMSE exceeds the acceptance threshold");
    }
    if (report.worst_mass_rmse_g > thresholds.median_mass_rmse_g * thresholds.worst_fold_multiplier) {
        report.failed_checks.emplace_back("a mass RMSE exceeds the worst-fold limit");
    }
    if (time_errors.size() == report.folds.size()) {
        if (report.median_time_error_s > thresholds.median_time_error_s) {
            report.failed_checks.emplace_back("median time error exceeds the acceptance threshold");
        }
        if (report.worst_time_error_s >
            thresholds.median_time_error_s * thresholds.worst_fold_multiplier) {
            report.failed_checks.emplace_back("a time error exceeds the worst-fold limit");
        }
    }
    if (report.tds_assessed) {
        if (*report.median_tds_error_percent > thresholds.median_tds_error_percent) {
            report.failed_checks.emplace_back("median TDS error exceeds the acceptance threshold");
        }
        if (*report.worst_tds_error_percent >
            thresholds.median_tds_error_percent * thresholds.worst_fold_multiplier) {
            report.failed_checks.emplace_back("a TDS error exceeds the worst-fold limit");
        }
    }
    report.passed = report.failed_checks.empty();
    return report;
}

}  // namespace espressolab::calibration
