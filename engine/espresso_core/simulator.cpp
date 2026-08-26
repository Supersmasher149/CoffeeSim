#include "espressolab/simulator.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "espressolab/extraction.hpp"
#include "espressolab/puck.hpp"
#include "espressolab/units.hpp"
#include "espressolab/version.hpp"

namespace espressolab {
namespace {

constexpr double kMassEpsilon = 1.0e-12;
constexpr double kSaturationTolerance = 1.0e-6;

struct Boundaries {
    double pressure_pa = 0.0;
    double delta_p_pa = 0.0;
    double inlet_temperature_k = 0.0;
};

struct Derived {
    PuckGeometry geometry;
    FlowSolution flow;
    double viscosity_pa_s = 0.0;
    double inlet_density_kg_m3 = 0.0;
    double water_heat_capacity_j_kg_k = 0.0;
    double permeability_m2 = 0.0;
    double pore_capacity_kg = 0.0;
};

struct RegionState {
    ShotState shot;
    double integrated_flow_m3 = 0.0;
};

class WarningLog {
public:
    void once(std::vector<SimulationWarning>& sink, const char* code, const char* message,
              double time_s, WarningSeverity severity) {
        for (const auto& existing : sink) {
            if (existing.code == code) return;
        }
        sink.push_back({code, message, time_s, severity});
    }
};

bool all_finite(const ShotState& state) {
    return std::isfinite(state.time_s) && std::isfinite(state.puck_temperature_k) &&
           std::isfinite(state.permeability_m2) && std::isfinite(state.liquid_saturation) &&
           std::isfinite(state.remaining_extractable_solids_kg) &&
           std::isfinite(state.dissolved_solids_kg) && std::isfinite(state.beverage_mass_kg) &&
           std::isfinite(state.cumulative_water_in_kg) && std::isfinite(state.retained_water_kg) &&
           std::isfinite(state.dissolved_solids_in_cup_kg);
}

bool all_finite(const std::vector<RegionState>& regions) {
    return std::all_of(regions.begin(), regions.end(), [](const RegionState& region) {
        return all_finite(region.shot) && std::isfinite(region.integrated_flow_m3);
    });
}

double tds_of(const ShotState& state) {
    return state.beverage_mass_kg > kMassEpsilon
               ? state.dissolved_solids_in_cup_kg / state.beverage_mass_kg
               : 0.0;
}

double yield_of(const ShotState& state, double dose_kg) {
    return dose_kg > kMassEpsilon ? state.dissolved_solids_in_cup_kg / dose_kg : 0.0;
}

ShotSample make_sample(const ShotState& state, const Boundaries& boundaries, double flow_m3_s,
                       const Recipe& recipe) {
    ShotSample sample;
    sample.time_s = state.time_s;
    sample.pressure_pa = boundaries.pressure_pa;
    sample.inlet_temperature_k = boundaries.inlet_temperature_k;
    sample.puck_temperature_k = state.puck_temperature_k;
    sample.flow_m3_s = flow_m3_s;
    sample.beverage_mass_kg = state.beverage_mass_kg;
    sample.tds_fraction = tds_of(state);
    sample.extraction_yield_fraction = yield_of(state, recipe.dose_kg);
    sample.saturation = state.liquid_saturation;
    sample.permeability_m2 = state.permeability_m2;
    return sample;
}

ShotState interpolate_state(const ShotState& lower, const ShotState& upper, double time_s) {
    const double span = upper.time_s - lower.time_s;
    const double fraction = span > 0.0 ? (time_s - lower.time_s) / span : 0.0;
    const auto interpolate = [fraction](double a, double b) { return a + fraction * (b - a); };
    ShotState state;
    state.time_s = time_s;
    state.puck_temperature_k = interpolate(lower.puck_temperature_k, upper.puck_temperature_k);
    state.permeability_m2 = interpolate(lower.permeability_m2, upper.permeability_m2);
    state.liquid_saturation = interpolate(lower.liquid_saturation, upper.liquid_saturation);
    state.remaining_extractable_solids_kg =
        interpolate(lower.remaining_extractable_solids_kg, upper.remaining_extractable_solids_kg);
    state.dissolved_solids_kg = interpolate(lower.dissolved_solids_kg, upper.dissolved_solids_kg);
    state.beverage_mass_kg = interpolate(lower.beverage_mass_kg, upper.beverage_mass_kg);
    state.cumulative_water_in_kg =
        interpolate(lower.cumulative_water_in_kg, upper.cumulative_water_in_kg);
    state.retained_water_kg = interpolate(lower.retained_water_kg, upper.retained_water_kg);
    state.dissolved_solids_in_cup_kg =
        interpolate(lower.dissolved_solids_in_cup_kg, upper.dissolved_solids_in_cup_kg);
    return state;
}

RegionState interpolate_region(const RegionState& lower, const RegionState& upper, double time_s) {
    const double span = upper.shot.time_s - lower.shot.time_s;
    const double fraction = span > 0.0 ? (time_s - lower.shot.time_s) / span : 0.0;
    RegionState state;
    state.shot = interpolate_state(lower.shot, upper.shot, time_s);
    state.integrated_flow_m3 =
        lower.integrated_flow_m3 + fraction * (upper.integrated_flow_m3 - lower.integrated_flow_m3);
    return state;
}

ShotState aggregate_state(const std::vector<RegionState>& regions, const std::vector<Derived>& derived,
                          const Recipe& recipe, const ModelCoefficients& coeff) {
    ShotState aggregate;
    aggregate.time_s = regions.front().shot.time_s;

    double thermal_capacity_sum = 0.0;
    double weighted_temperature = 0.0;
    double pore_capacity_sum = 0.0;
    double weighted_saturation = 0.0;
    double weighted_permeability = 0.0;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const ShotState& state = regions[i].shot;
        const Derived& region = derived[i];
        aggregate.remaining_extractable_solids_kg += state.remaining_extractable_solids_kg;
        aggregate.dissolved_solids_kg += state.dissolved_solids_kg;
        aggregate.beverage_mass_kg += state.beverage_mass_kg;
        aggregate.cumulative_water_in_kg += state.cumulative_water_in_kg;
        aggregate.retained_water_kg += state.retained_water_kg;
        aggregate.dissolved_solids_in_cup_kg += state.dissolved_solids_in_cup_kg;

        const double thermal_capacity =
            recipe.dose_kg * recipe.parallel_regions[i].area_fraction *
                coeff.coffee_heat_capacity_j_kg_k +
            std::max(state.retained_water_kg, kMassEpsilon) * region.water_heat_capacity_j_kg_k;
        thermal_capacity_sum += thermal_capacity;
        weighted_temperature += state.puck_temperature_k * thermal_capacity;
        pore_capacity_sum += region.pore_capacity_kg;
        weighted_saturation += state.liquid_saturation * region.pore_capacity_kg;
        weighted_permeability +=
            region.permeability_m2 * recipe.parallel_regions[i].area_fraction;
    }
    aggregate.puck_temperature_k = thermal_capacity_sum > kMassEpsilon
                                      ? weighted_temperature / thermal_capacity_sum
                                      : regions.front().shot.puck_temperature_k;
    aggregate.liquid_saturation = pore_capacity_sum > kMassEpsilon
                                      ? weighted_saturation / pore_capacity_sum
                                      : regions.front().shot.liquid_saturation;
    aggregate.permeability_m2 = weighted_permeability;
    return aggregate;
}

double total_flow(const std::vector<Derived>& derived) {
    double flow = 0.0;
    for (const Derived& region : derived) flow += region.flow.flow_m3_s;
    return flow;
}

double total_integrated_flow(const std::vector<RegionState>& regions) {
    double flow = 0.0;
    for (const RegionState& region : regions) flow += region.integrated_flow_m3;
    return flow;
}

}  // namespace

InvalidInputError::InvalidInputError(const ValidationResult& result)
    : std::runtime_error("invalid simulation input: " + result.summary()), validation_(result) {}

Simulator::Simulator() : water_(std::make_shared<TabulatedWaterProperties>()) {}

Simulator::Simulator(std::shared_ptr<const WaterProperties> water) : water_(std::move(water)) {
    if (!water_) water_ = std::make_shared<TabulatedWaterProperties>();
}

ShotResult Simulator::run(const Recipe& recipe, const ModelCoefficients& coeff,
                           const SimulationConfig& config) const {
    ValidationResult validation = recipe.validate();
    validation.merge(coeff.validate());
    if (config.dt_s <= 0.0 || config.sample_interval_s <= 0.0) {
        validation.add("NONPHYSICAL_INPUT", "solver dt_s and sample_interval_s must be positive",
                       "config.dt_s");
    }
    if (!validation.ok()) throw InvalidInputError(validation);

    ShotResult result;
    WarningLog warn;
    std::vector<RegionState> regions(recipe.parallel_regions.size());
    for (RegionState& region : regions) {
        region.shot.puck_temperature_k = coeff.initial_puck_temperature_k;
    }
    for (std::size_t i = 0; i < regions.size(); ++i) {
        regions[i].shot.remaining_extractable_solids_kg =
            recipe.dose_kg * recipe.parallel_regions[i].area_fraction * coeff.extractable_solids_fraction;
    }
    const double initial_extractable_kg = recipe.dose_kg * coeff.extractable_solids_fraction;

    ShotDiagnostics& diag = result.diagnostics;
    diag.min_permeability_m2 = std::numeric_limits<double>::max();
    diag.min_puck_temperature_k = coeff.initial_puck_temperature_k;
    diag.max_puck_temperature_k = coeff.initial_puck_temperature_k;

    const double area_m2 = recipe.basket_area_m2();
    const double dt = config.dt_s;
    TerminationReason termination = TerminationReason::not_terminated;
    double next_sample_time_s = 0.0;

    const auto evaluate = [&](const std::vector<RegionState>& states) {
        Boundaries boundaries;
        boundaries.pressure_pa = recipe.pressure_pa.sample(states.front().shot.time_s);
        boundaries.inlet_temperature_k = recipe.inlet_temperature_k.sample(states.front().shot.time_s);
        boundaries.delta_p_pa = boundaries.pressure_pa - coeff.outlet_pressure_pa;

        std::vector<Derived> derived;
        derived.reserve(states.size());
        for (std::size_t i = 0; i < states.size(); ++i) {
            const ShotState& state = states[i].shot;
            const ParallelRegion& region = recipe.parallel_regions[i];
            Derived d;
            d.viscosity_pa_s = water_->viscosity_pa_s(state.puck_temperature_k);
            d.inlet_density_kg_m3 = water_->density_kg_m3(boundaries.inlet_temperature_k);
            d.water_heat_capacity_j_kg_k = water_->heat_capacity_j_kg_k(state.puck_temperature_k);
            d.geometry = compress_puck(recipe, coeff, boundaries.delta_p_pa);
            const double k0 = kozeny_carman_permeability(recipe.particle_diameter_m,
                                                          d.geometry.porosity, coeff.kozeny_constant);
            d.permeability_m2 =
                k0 * distribution_factor(recipe.particle_spread_factor, coeff.distribution_factor_floor) *
                wetting_factor(state.liquid_saturation, coeff.dry_permeability_multiplier) *
                region.permeability_multiplier;
            d.flow = darcy_flow(d.permeability_m2, area_m2 * region.area_fraction,
                                d.viscosity_pa_s, d.geometry.depth_m, boundaries.delta_p_pa,
                                coeff.maximum_flow_m3_s);
            d.pore_capacity_kg = area_m2 * region.area_fraction * d.geometry.depth_m *
                                 d.geometry.porosity *
                                 water_->density_kg_m3(state.puck_temperature_k);
            derived.push_back(d);
        }
        return std::pair{boundaries, derived};
    };

    for (long long step = 0;; ++step) {
        const auto [boundaries, derived] = evaluate(regions);
        for (std::size_t i = 0; i < regions.size(); ++i) {
            regions[i].shot.permeability_m2 = derived[i].permeability_m2;
            diag.min_permeability_m2 = std::min(diag.min_permeability_m2, derived[i].permeability_m2);
            if (derived[i].flow.clamped_by_max_flow) {
                ++diag.clamp_count;
                warn.once(result.warnings, "FLOW_CLAMPED",
                          "flow hit the numerical maximum; result is a guard value, not a prediction",
                          regions[i].shot.time_s, WarningSeverity::hard);
            }
        }
        const double flow_m3_s = total_flow(derived);
        diag.max_flow_m3_s = std::max(diag.max_flow_m3_s, flow_m3_s);

        if (boundaries.inlet_temperature_k > water_->max_temperature_k() ||
            boundaries.inlet_temperature_k < water_->min_temperature_k()) {
            ++diag.clamp_count;
            warn.once(result.warnings, "TEMPERATURE_OUT_OF_TABLE",
                      "inlet temperature is outside the water property table range",
                      regions.front().shot.time_s, WarningSeverity::hard);
        }

        const ShotState aggregate = aggregate_state(regions, derived, recipe, coeff);
        if (aggregate.time_s + 1.0e-9 >= next_sample_time_s) {
            result.samples.push_back(make_sample(aggregate, boundaries, flow_m3_s, recipe));
            next_sample_time_s += config.sample_interval_s;
        }

        if (!all_finite(regions)) {
            termination = TerminationReason::numerical_failure;
            result.warnings.push_back({"NUMERICAL_FAILURE", "state contained a non-finite value",
                                       aggregate.time_s, WarningSeverity::hard});
            break;
        }
        if (recipe.target_beverage_mass_kg.has_value() &&
            aggregate.beverage_mass_kg >= *recipe.target_beverage_mass_kg) {
            termination = TerminationReason::target_mass_reached;
            break;
        }
        if (aggregate.time_s >= recipe.maximum_time_s) {
            termination = TerminationReason::time_limit_reached;
            break;
        }

        const std::vector<RegionState> states_before_step = regions;
        bool saturation_invalid = false;
        for (std::size_t i = 0; i < regions.size(); ++i) {
            RegionState& region = regions[i];
            ShotState& state = region.shot;
            const Derived& d = derived[i];

            const double water_in_kg = d.flow.flow_m3_s * d.inlet_density_kg_m3 * dt;
            region.integrated_flow_m3 += d.flow.flow_m3_s * dt;
            state.cumulative_water_in_kg += water_in_kg;
            state.retained_water_kg += water_in_kg;

            const double thermal_capacity_j_k =
                recipe.dose_kg * recipe.parallel_regions[i].area_fraction *
                    coeff.coffee_heat_capacity_j_kg_k +
                std::max(state.retained_water_kg, kMassEpsilon) * d.water_heat_capacity_j_kg_k;
            const double mass_flow_kg_s = d.flow.flow_m3_s * d.inlet_density_kg_m3;
            const double heat_in_w = mass_flow_kg_s * d.water_heat_capacity_j_kg_k *
                                     (boundaries.inlet_temperature_k - state.puck_temperature_k);
            const double heat_loss_w =
                coeff.ambient_heat_loss_w_k * (state.puck_temperature_k - coeff.ambient_temperature_k);
            const double dT_dt = (heat_in_w - heat_loss_w) / thermal_capacity_j_k;
            state.puck_temperature_k += dT_dt * dt;
            if (std::abs(dT_dt * dt) > 5.0) {
                warn.once(result.warnings, "TEMPERATURE_STEP_LARGE",
                          "puck temperature moved more than 5 K in one step; reduce dt_s",
                          state.time_s, WarningSeverity::soft);
            }
            state.puck_temperature_k = std::clamp(state.puck_temperature_k,
                                                   water_->min_temperature_k(),
                                                   water_->max_temperature_k());
            diag.min_puck_temperature_k = std::min(diag.min_puck_temperature_k, state.puck_temperature_k);
            diag.max_puck_temperature_k = std::max(diag.max_puck_temperature_k, state.puck_temperature_k);

            const double k_ext = extraction_rate_coefficient(state, recipe, coeff, d.flow.flow_m3_s);
            double extracted_kg = k_ext * state.remaining_extractable_solids_kg * dt;
            extracted_kg = std::clamp(extracted_kg, 0.0, state.remaining_extractable_solids_kg);
            state.remaining_extractable_solids_kg -= extracted_kg;
            state.dissolved_solids_kg += extracted_kg;
            state.retained_water_kg += extracted_kg;

            const double capacity_kg = std::max(d.pore_capacity_kg, kMassEpsilon);
            double out_kg = std::max(state.retained_water_kg - capacity_kg, 0.0);
            out_kg = std::min(out_kg, state.retained_water_kg);
            if (out_kg > 0.0) {
                const double c_pore =
                    state.dissolved_solids_kg / std::max(state.retained_water_kg, kMassEpsilon);
                const double solids_out_kg = std::min(out_kg * c_pore, state.dissolved_solids_kg);
                state.dissolved_solids_kg -= solids_out_kg;
                state.dissolved_solids_in_cup_kg += solids_out_kg;
                state.retained_water_kg -= out_kg;
                state.beverage_mass_kg += out_kg;
            }

            state.liquid_saturation = state.retained_water_kg / capacity_kg;
            if (state.liquid_saturation > 1.0 + kSaturationTolerance ||
                state.liquid_saturation < -kSaturationTolerance) {
                if (config.strict_invariants) {
                    termination = TerminationReason::invalid_state;
                    result.warnings.push_back({"SATURATION_INVARIANT",
                                               "liquid saturation left [0, 1] beyond tolerance",
                                               state.time_s, WarningSeverity::hard});
                    saturation_invalid = true;
                    break;
                }
                ++diag.clamp_count;
            }
            state.liquid_saturation = std::clamp(state.liquid_saturation, 0.0, 1.0);
        }
        if (saturation_invalid) break;

        for (RegionState& region : regions) {
            region.shot.time_s = static_cast<double>(step + 1) * dt;
        }
        diag.step_count = step + 1;

        while (next_sample_time_s <= regions.front().shot.time_s + 1.0e-9) {
            std::vector<RegionState> sampled_regions;
            sampled_regions.reserve(regions.size());
            for (std::size_t i = 0; i < regions.size(); ++i) {
                sampled_regions.push_back(
                    interpolate_region(states_before_step[i], regions[i], next_sample_time_s));
            }
            const auto [sampled_boundaries, sampled_derived] = evaluate(sampled_regions);
            for (std::size_t i = 0; i < sampled_regions.size(); ++i) {
                sampled_regions[i].shot.permeability_m2 = sampled_derived[i].permeability_m2;
            }
            const ShotState sampled = aggregate_state(sampled_regions, sampled_derived, recipe, coeff);
            result.samples.push_back(
                make_sample(sampled, sampled_boundaries, total_flow(sampled_derived), recipe));
            next_sample_time_s += config.sample_interval_s;
        }
    }

    const auto [final_boundaries, final_derived] = evaluate(regions);
    for (std::size_t i = 0; i < regions.size(); ++i) {
        regions[i].shot.permeability_m2 = final_derived[i].permeability_m2;
    }
    const ShotState final_state = aggregate_state(regions, final_derived, recipe, coeff);

    const double solids_total_kg =
        final_state.dissolved_solids_kg + final_state.dissolved_solids_in_cup_kg;
    diag.water_mass_residual_kg = final_state.cumulative_water_in_kg + solids_total_kg -
                                  (final_state.retained_water_kg + final_state.beverage_mass_kg);
    diag.solids_mass_residual_kg =
        (initial_extractable_kg - final_state.remaining_extractable_solids_kg) - solids_total_kg;
    if (diag.min_permeability_m2 == std::numeric_limits<double>::max()) {
        diag.min_permeability_m2 = 0.0;
    }

    if (result.samples.empty() || result.samples.back().time_s < final_state.time_s - 1.0e-9) {
        result.samples.push_back(
            make_sample(final_state, final_boundaries, total_flow(final_derived), recipe));
    }

    const double integrated_flow_m3 = total_integrated_flow(regions);
    result.regions.reserve(regions.size());
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const ShotState& state = regions[i].shot;
        const ParallelRegion& config_region = recipe.parallel_regions[i];
        result.regions.push_back({config_region.area_fraction,
                                  config_region.permeability_multiplier,
                                  state.beverage_mass_kg,
                                  integrated_flow_m3 > kMassEpsilon
                                      ? regions[i].integrated_flow_m3 / integrated_flow_m3
                                      : 0.0,
                                  tds_of(state),
                                  yield_of(state, recipe.dose_kg * config_region.area_fraction)});
    }

    ShotSummary& summary = result.summary;
    summary.termination = termination;
    summary.elapsed_time_s = final_state.time_s;
    summary.target_mass_reached = termination == TerminationReason::target_mass_reached;
    summary.beverage_mass_kg = final_state.beverage_mass_kg;
    summary.tds_fraction = tds_of(final_state);
    summary.extraction_yield_fraction = yield_of(final_state, recipe.dose_kg);
    summary.brew_ratio = recipe.dose_kg > kMassEpsilon
                             ? final_state.beverage_mass_kg / recipe.dose_kg
                             : 0.0;
    summary.peak_flow_m3_s = diag.max_flow_m3_s;
    summary.average_flow_m3_s =
        final_state.time_s > 0.0 ? integrated_flow_m3 / final_state.time_s : 0.0;
    summary.warning_count = static_cast<int>(result.warnings.size());

    if (summary.beverage_mass_kg <= kMassEpsilon) {
        result.warnings.push_back({"NO_BEVERAGE_PRODUCED",
                                   "the puck never saturated enough to release beverage",
                                   final_state.time_s, WarningSeverity::hard});
        summary.warning_count = static_cast<int>(result.warnings.size());
    }

    result.manifest.solver_version = std::string(version::kSolver);
    result.manifest.result_schema_version = std::string(version::kResultSchema);
    result.manifest.coefficient_id = coeff.id;
    result.manifest.coefficient_version = coeff.version;
    result.manifest.dt_s = config.dt_s;
    result.manifest.sample_interval_s = config.sample_interval_s;
    return result;
}

}  // namespace espressolab
