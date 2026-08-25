#include <algorithm>
#include <cmath>
#include <numeric>

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

}  // namespace

CalibrationReport fit(const CalibrationSpec& spec) {
    ValidationResult validation;
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
            const std::vector<double> contracted = combine(-kContraction);
            const double contracted_value = objective(contracted);
            if (contracted_value < values.back()) {
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

}  // namespace espressolab::calibration
