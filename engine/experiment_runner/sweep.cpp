#include "espressolab/experiment.hpp"

#include <limits>
#include <map>
#include <set>

#include "espressolab/artifact_io.hpp"
#include "espressolab/units.hpp"

namespace espressolab {
namespace {

// Sweep axes address the recipe by its dashboard-facing path and unit, so a
// sweep file reads the same way as the recipe file it perturbs (11.2).
using Setter = void (*)(Recipe&, double);

// apps/espressolab_server/main.cpp's POST /api/v1/sweeps rejects a sweep
// whose Cartesian product exceeds 20,000 runs, but that check lives only in
// the REST handler -- espressolab_cli sweep calls ExperimentRunner::run (or
// the parallel batch runner) with no equivalent limit, and
// ExperimentRunner::run reserves a std::vector<SweepRun> sized to the full
// product up front. Three "range" axes at steps=10000 each -- a plausible
// typo, not even a deliberately adversarial spec -- is a 10^12-run product
// that reserve() immediately fails to allocate (caught, but reported as an
// opaque "INTERNAL_ERROR: std::bad_alloc"); a somewhat smaller product would
// instead succeed and run for an unbounded, unannounced amount of wall time.
// Both CLI paths call validate_sweep_spec() first, so one cap here protects
// both. Kept well above REST's tighter 20,000 (a local CLI sweep is a
// legitimate place to run a much larger batch than a shared server request
// should be allowed to start) but far below what the reserve()/compute cost
// of a genuinely pathological spec would need.
constexpr std::size_t kMaxSweepRuns = 1'000'000;

const std::map<std::string, Setter>& setters() {
    static const std::map<std::string, Setter> table{
        {"puck.dose_g", [](Recipe& r, double v) { r.dose_kg = units::grams_to_kg(v); }},
        {"puck.basket_diameter_mm",
         [](Recipe& r, double v) { r.basket_diameter_m = units::mm_to_m(v); }},
        {"puck.depth_mm", [](Recipe& r, double v) { r.puck_depth_m = units::mm_to_m(v); }},
        // On a distribution-bearing recipe the scalar is derived, so writing it
        // directly would leave particle_diameter_m disagreeing with the bins the
        // solver still extracts from. Scale the whole distribution instead:
        // multiplying every bin diameter by a constant leaves ln-variance -- and
        // so the spread -- untouched, and d32 is homogeneous of degree one, so
        // the scaled distribution lands exactly on the requested diameter. That
        // is precisely a grind-size sweep: one shape, moved finer or coarser.
        {"puck.particle_diameter_um",
         [](Recipe& r, double v) {
             const double target_m = units::microns_to_m(v);
             if (r.grind.has_value()) {
                 const double current_m = r.grind->sauter_mean_diameter_m();
                 if (!(current_m > 0.0)) {
                     ValidationResult result;
                     result.add("NONPHYSICAL_INPUT",
                                "cannot sweep puck.particle_diameter_um: the baseline recipe's "
                                "grind distribution has no positive Sauter mean diameter",
                                "recipe.puck.grind.bins");
                     throw InvalidInputError(result);
                 }
                 const double scale = target_m / current_m;
                 for (GrindBin& bin : r.grind->bins) bin.diameter_m *= scale;
                 // Re-derive rather than assign the target, so the invariant the
                 // loader establishes -- the scalar is exactly the bins' d32 --
                 // survives a sweep instead of holding only to within an ulp.
                 r.particle_diameter_m = r.grind->sauter_mean_diameter_m();
                 r.particle_spread_factor = r.grind->equivalent_spread_factor();
                 return;
             }
             r.particle_diameter_m = target_m;
         }},
        {"puck.particle_spread_factor",
         [](Recipe& r, double v) {
             // Unlike the diameter there is no shape-preserving way to retarget
             // the spread of a fixed distribution -- any answer would be an
             // invented reshaping. Refuse rather than pick one silently.
             if (r.grind.has_value()) {
                 ValidationResult result;
                 result.add("UNSUPPORTED_PARAMETER",
                            "puck.particle_spread_factor cannot be swept on a recipe that carries "
                            "a grind distribution: the spread is derived from the bins. Sweep "
                            "puck.particle_diameter_um, or use a scalar recipe.",
                            "recipe.puck.grind");
                 throw InvalidInputError(result);
             }
             r.particle_spread_factor = v;
         }},
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

void validate_sweep_spec(const SweepSpec& spec) {
    if (spec.axes.empty()) {
        ValidationResult result;
        result.add("EMPTY_SWEEP", "a sweep requires at least one axis", "sweep.axes");
        throw InvalidInputError(result);
    }
    std::set<std::string> parameter_paths;
    // Accumulated the overflow-safe way (checked before each multiply, not
    // after) so a product that wraps std::size_t can't slip under
    // kMaxSweepRuns and look small.
    std::size_t total_runs = 1;
    bool total_runs_overflowed = false;
    for (const auto& axis : spec.axes) {
        if (axis.values.empty()) {
            ValidationResult result;
            result.add("EMPTY_SWEEP_AXIS", "axis '" + axis.parameter_path + "' has no values",
                       "sweep.axes." + axis.parameter_path);
            throw InvalidInputError(result);
        }
        if (!parameter_paths.insert(axis.parameter_path).second) {
            ValidationResult result;
            result.add("DUPLICATE_SWEEP_AXIS",
                       "parameter '" + axis.parameter_path + "' appears in more than one axis",
                       "sweep.axes." + axis.parameter_path);
            throw InvalidInputError(result);
        }
        // Fail before running a hundred simulations rather than after.
        (void)apply_parameter(spec.baseline, axis.parameter_path, axis.values.front());

        if (!total_runs_overflowed) {
            if (axis.values.size() != 0 &&
                total_runs > std::numeric_limits<std::size_t>::max() / axis.values.size()) {
                total_runs_overflowed = true;
            } else {
                total_runs *= axis.values.size();
            }
        }
    }
    if (total_runs_overflowed || total_runs > kMaxSweepRuns) {
        ValidationResult result;
        result.add("SWEEP_TOO_LARGE",
                   total_runs_overflowed
                       ? "sweep's axes multiply to more runs than can be counted, exceeding the " +
                             std::to_string(kMaxSweepRuns) + "-run limit"
                       : "sweep requests " + std::to_string(total_runs) +
                             " runs, exceeding the " + std::to_string(kMaxSweepRuns) +
                             "-run limit",
                   "sweep.axes");
        throw InvalidInputError(result);
    }
}

std::size_t sweep_total_runs(const SweepSpec& spec) {
    // Cartesian product with the last axis varying fastest, so run order is
    // stable across machines and reruns (14.2).
    std::size_t total = 1;
    for (const auto& axis : spec.axes) total *= axis.values.size();
    return total;
}

std::vector<double> sweep_coordinates(const SweepSpec& spec, std::size_t linear_index) {
    std::size_t remainder = linear_index;
    std::vector<double> coordinates(spec.axes.size(), 0.0);
    for (std::size_t axis_index = spec.axes.size(); axis_index-- > 0;) {
        const auto& values = spec.axes[axis_index].values;
        coordinates[axis_index] = values[remainder % values.size()];
        remainder /= values.size();
    }
    return coordinates;
}

SweepRun execute_sweep_point(const SweepSpec& spec, const Simulator& simulator,
                              std::size_t linear_index) {
    const std::vector<double> coordinates = sweep_coordinates(spec, linear_index);

    Recipe recipe = spec.baseline;
    for (std::size_t axis_index = 0; axis_index < spec.axes.size(); ++axis_index) {
        recipe = apply_parameter(recipe, spec.axes[axis_index].parameter_path,
                                 coordinates[axis_index]);
    }

    SweepRun run;
    run.index = static_cast<int>(linear_index);
    run.coordinates = coordinates;

    // One out-of-range corner must not abandon the other runs (FR-05); it is
    // recorded as an invalid run and shows up in the aggregate.
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
    return run;
}

SweepResult ExperimentRunner::run(const SweepSpec& spec,
                                  const SweepProgressCallback& on_progress) const {
    validate_sweep_spec(spec);

    SweepResult result;
    result.name = spec.name;
    result.axes = spec.axes;

    const std::size_t total = sweep_total_runs(spec);
    const Simulator simulator;
    result.runs.reserve(total);
    for (std::size_t linear = 0; linear < total; ++linear) {
        result.runs.push_back(execute_sweep_point(spec, simulator, linear));

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
