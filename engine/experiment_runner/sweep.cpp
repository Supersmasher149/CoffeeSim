#include "espressolab/experiment.hpp"

#include <algorithm>
#include <map>

#include "espressolab/artifact_io.hpp"
#include "espressolab/units.hpp"

namespace espressolab {
namespace {

// Sweep axes address the recipe by its dashboard-facing path and unit, so a
// sweep file reads the same way as the recipe file it perturbs (11.2).
using Setter = void (*)(Recipe&, double);

const std::map<std::string, Setter>& setters() {
    static const std::map<std::string, Setter> table{
        {"puck.dose_g", [](Recipe& r, double v) { r.dose_kg = units::grams_to_kg(v); }},
        {"puck.basket_diameter_mm",
         [](Recipe& r, double v) { r.basket_diameter_m = units::mm_to_m(v); }},
        {"puck.depth_mm", [](Recipe& r, double v) { r.puck_depth_m = units::mm_to_m(v); }},
        {"puck.particle_diameter_um",
         [](Recipe& r, double v) { r.particle_diameter_m = units::microns_to_m(v); }},
        {"puck.particle_spread_factor",
         [](Recipe& r, double v) { r.particle_spread_factor = v; }},
        {"stop.target_beverage_g",
         [](Recipe& r, double v) { r.target_beverage_mass_kg = units::grams_to_kg(v); }},
        {"stop.maximum_time_s", [](Recipe& r, double v) { r.maximum_time_s = v; }},
        // Replaces the whole inlet profile with a constant, which is the common
        // temperature sweep and keeps the axis one-dimensional.
        {"temperature_profile_c.constant",
         [](Recipe& r, double v) {
             r.inlet_temperature_k = PiecewiseLinearProfile::constant(units::celsius_to_kelvin(v));
         }},
        // Scales every pressure point, preserving the shape of a ramp or a
        // declining profile.
        {"pressure_profile_bar.scale",
         [](Recipe& r, double v) {
             std::vector<ProfilePoint> points = r.pressure_pa.points();
             for (auto& point : points) point.value *= v;
             r.pressure_pa = PiecewiseLinearProfile(std::move(points));
         }},
    };
    return table;
}

}  // namespace

std::vector<std::string> supported_parameter_paths() {
    std::vector<std::string> paths;
    paths.reserve(setters().size());
    for (const auto& [path, _] : setters()) paths.push_back(path);
    return paths;
}

Recipe apply_parameter(const Recipe& baseline, const std::string& parameter_path, double value) {
    const auto it = setters().find(parameter_path);
    if (it == setters().end()) {
        ValidationResult result;
        result.add("UNKNOWN_PARAMETER_PATH", "no sweepable parameter named '" + parameter_path + "'",
                   parameter_path);
        throw InvalidInputError(result);
    }
    Recipe copy = baseline;
    it->second(copy, value);
    return copy;
}

SweepResult ExperimentRunner::run(const SweepSpec& spec,
                                  const SweepProgressCallback& on_progress) const {
    if (spec.axes.empty()) {
        ValidationResult result;
        result.add("EMPTY_SWEEP", "a sweep requires at least one axis", "sweep.axes");
        throw InvalidInputError(result);
    }
    for (const auto& axis : spec.axes) {
        if (axis.values.empty()) {
            ValidationResult result;
            result.add("EMPTY_SWEEP_AXIS", "axis '" + axis.parameter_path + "' has no values",
                       "sweep.axes." + axis.parameter_path);
            throw InvalidInputError(result);
        }
        // Fail before running a hundred simulations rather than after.
        (void)apply_parameter(spec.baseline, axis.parameter_path, axis.values.front());
    }

    SweepResult result;
    result.name = spec.name;
    result.axes = spec.axes;

    // Cartesian product with the last axis varying fastest, so run order is
    // stable across machines and reruns (14.2).
    std::size_t total = 1;
    for (const auto& axis : spec.axes) total *= axis.values.size();

    const Simulator simulator;
    result.runs.reserve(total);
    for (std::size_t linear = 0; linear < total; ++linear) {
        std::size_t remainder = linear;
        std::vector<double> coordinates(spec.axes.size(), 0.0);
        for (std::size_t axis_index = spec.axes.size(); axis_index-- > 0;) {
            const auto& values = spec.axes[axis_index].values;
            coordinates[axis_index] = values[remainder % values.size()];
            remainder /= values.size();
        }

        Recipe recipe = spec.baseline;
        for (std::size_t axis_index = 0; axis_index < spec.axes.size(); ++axis_index) {
            recipe = apply_parameter(recipe, spec.axes[axis_index].parameter_path,
                                     coordinates[axis_index]);
        }

        SweepRun run;
        run.index = static_cast<int>(linear);
        run.coordinates = coordinates;

        // One out-of-range corner must not abandon the other 99 runs (FR-05);
        // it is recorded as an invalid run and shows up in the aggregate.
        try {
            ShotResult shot = simulator.run(recipe, spec.coefficients, spec.config);
            artifact_io::stamp_manifest(shot, recipe, spec.coefficients, spec.config);
            run.summary = shot.summary;
            run.run_id = shot.manifest.run_id;
            run.result_hash = shot.manifest.result_hash;
            run.warning_count = shot.summary.warning_count;
        } catch (const InvalidInputError& e) {
            run.summary.termination = TerminationReason::invalid_state;
            run.run_id = "invalid";
            run.result_hash.clear();
            run.warning_count = static_cast<int>(e.validation().issues().size());
        }
        result.runs.push_back(std::move(run));

        if (on_progress && !on_progress(static_cast<int>(linear + 1), static_cast<int>(total))) {
            // A cancelled sweep keeps the runs it already finished rather than
            // discarding work the caller may still want.
            result.cancelled = true;
            break;
        }
    }

    result.sweep_id = "sweep-" + spec.name;
    return result;
}

}  // namespace espressolab
