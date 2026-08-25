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

// Boundary values sampled at the current simulation time (9.2, step 1).
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

class WarningLog {
public:
    void once(std::vector<SimulationWarning>& sink, const char* code, const char* message,
              double time_s, WarningSeverity severity) {
        for (const auto& existing : sink) {
            if (existing.code == code) return;  // one entry per condition, first occurrence wins
        }
        sink.push_back({code, message, time_s, severity});
    }
};

bool all_finite(const ShotState& s) {
    return std::isfinite(s.time_s) && std::isfinite(s.puck_temperature_k) &&
           std::isfinite(s.permeability_m2) && std::isfinite(s.liquid_saturation) &&
           std::isfinite(s.remaining_extractable_solids_kg) &&
           std::isfinite(s.dissolved_solids_kg) && std::isfinite(s.beverage_mass_kg) &&
           std::isfinite(s.cumulative_water_in_kg) && std::isfinite(s.retained_water_kg) &&
           std::isfinite(s.dissolved_solids_in_cup_kg);
}

double tds_of(const ShotState& s) {
    return s.beverage_mass_kg > kMassEpsilon ? s.dissolved_solids_in_cup_kg / s.beverage_mass_kg
                                             : 0.0;
}

// 8.1: extraction yield is reported from the solids that actually reached the
// cup, which is what a refractometer measurement can be compared against.
double yield_of(const ShotState& s, const Recipe& recipe) {
    return recipe.dose_kg > kMassEpsilon ? s.dissolved_solids_in_cup_kg / recipe.dose_kg : 0.0;
}

ShotSample make_sample(const ShotState& s, const Boundaries& b, const Derived& d,
                       const Recipe& recipe) {
    ShotSample sample;
    sample.time_s = s.time_s;
    sample.pressure_pa = b.pressure_pa;
    sample.inlet_temperature_k = b.inlet_temperature_k;
    sample.puck_temperature_k = s.puck_temperature_k;
    sample.flow_m3_s = d.flow.flow_m3_s;
    sample.beverage_mass_kg = s.beverage_mass_kg;
    sample.tds_fraction = tds_of(s);
    sample.extraction_yield_fraction = yield_of(s, recipe);
    sample.saturation = s.liquid_saturation;
    sample.permeability_m2 = s.permeability_m2;
    return sample;
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

    // --- Initial state (4.4)
    ShotState state;
    state.puck_temperature_k = coeff.initial_puck_temperature_k;
    state.remaining_extractable_solids_kg = recipe.dose_kg * coeff.extractable_solids_fraction;
    const double initial_extractable_kg = state.remaining_extractable_solids_kg;

    ShotDiagnostics& diag = result.diagnostics;
    diag.min_permeability_m2 = std::numeric_limits<double>::max();
    diag.min_puck_temperature_k = state.puck_temperature_k;
    diag.max_puck_temperature_k = state.puck_temperature_k;

    const double area_m2 = recipe.basket_area_m2();
    const double dt = config.dt_s;
    TerminationReason termination = TerminationReason::not_terminated;
    double next_sample_time_s = 0.0;
    Derived derived_at_sample;
    Boundaries boundaries_at_sample;

    const auto evaluate = [&](const ShotState& s) {
        // 9.2 steps 1-3.
        Boundaries b;
        b.pressure_pa = recipe.pressure_pa.sample(s.time_s);
        b.inlet_temperature_k = recipe.inlet_temperature_k.sample(s.time_s);
        b.delta_p_pa = b.pressure_pa - coeff.outlet_pressure_pa;

        Derived d;
        d.viscosity_pa_s = water_->viscosity_pa_s(s.puck_temperature_k);
        d.inlet_density_kg_m3 = water_->density_kg_m3(b.inlet_temperature_k);
        d.water_heat_capacity_j_kg_k = water_->heat_capacity_j_kg_k(s.puck_temperature_k);
        d.geometry = compress_puck(recipe, coeff, b.delta_p_pa);

        const double k0 = kozeny_carman_permeability(recipe.particle_diameter_m,
                                                     d.geometry.porosity, coeff.kozeny_constant);
        constexpr double kChannelFactor = 1.0;  // exactly 1.0 in the MVP uniform puck (6.2)
        d.permeability_m2 = k0 *
                            distribution_factor(recipe.particle_spread_factor,
                                                coeff.distribution_factor_floor) *
                            wetting_factor(s.liquid_saturation, coeff.dry_permeability_multiplier) *
                            kChannelFactor;

        // 9.2 step 4.
        d.flow = darcy_flow(d.permeability_m2, area_m2, d.viscosity_pa_s, d.geometry.depth_m,
                            b.delta_p_pa, coeff.maximum_flow_m3_s);
        d.pore_capacity_kg =
            d.geometry.pore_volume_m3 * water_->density_kg_m3(s.puck_temperature_k);
        return std::pair{b, d};
    };

    for (long long step = 0;; ++step) {
        const auto [b, d] = evaluate(state);
        state.permeability_m2 = d.permeability_m2;
        boundaries_at_sample = b;
        derived_at_sample = d;

        diag.min_permeability_m2 = std::min(diag.min_permeability_m2, d.permeability_m2);
        diag.max_flow_m3_s = std::max(diag.max_flow_m3_s, d.flow.flow_m3_s);

        if (d.flow.clamped_by_max_flow) {
            ++diag.clamp_count;
            warn.once(result.warnings, "FLOW_CLAMPED",
                      "flow hit the numerical maximum; result is a guard value, not a prediction",
                      state.time_s, WarningSeverity::hard);
        }
        if (b.inlet_temperature_k > water_->max_temperature_k() ||
            b.inlet_temperature_k < water_->min_temperature_k()) {
            ++diag.clamp_count;
            warn.once(result.warnings, "TEMPERATURE_OUT_OF_TABLE",
                      "inlet temperature is outside the water property table range", state.time_s,
                      WarningSeverity::hard);
        }

        // --- Sampling at the requested interval (9.1)
        if (state.time_s + 1.0e-9 >= next_sample_time_s) {
            result.samples.push_back(make_sample(state, b, d, recipe));
            next_sample_time_s += config.sample_interval_s;
        }

        // --- Termination checks (9.2, step 8)
        if (!all_finite(state)) {
            termination = TerminationReason::numerical_failure;
            result.warnings.push_back({"NUMERICAL_FAILURE", "state contained a non-finite value",
                                       state.time_s, WarningSeverity::hard});
            break;
        }
        if (recipe.target_beverage_mass_kg.has_value() &&
            state.beverage_mass_kg >= *recipe.target_beverage_mass_kg) {
            termination = TerminationReason::target_mass_reached;
            break;
        }
        if (state.time_s >= recipe.maximum_time_s) {
            termination = TerminationReason::time_limit_reached;
            break;
        }

        // --- Integrate one explicit step -----------------------------------
        // 9.2 step 4: inflow, pore filling and outflow.
        const double water_in_kg = d.flow.flow_m3_s * d.inlet_density_kg_m3 * dt;
        state.cumulative_water_in_kg += water_in_kg;
        state.retained_water_kg += water_in_kg;  // pore liquid: water + dissolved solids

        // 9.2 step 5: lumped puck temperature (7.2/7.3).
        const double mass_flow_kg_s = d.flow.flow_m3_s * d.inlet_density_kg_m3;
        const double thermal_capacity_j_k =
            recipe.dose_kg * coeff.coffee_heat_capacity_j_kg_k +
            std::max(state.retained_water_kg, kMassEpsilon) * d.water_heat_capacity_j_kg_k;
        const double heat_in_w =
            mass_flow_kg_s * d.water_heat_capacity_j_kg_k * (b.inlet_temperature_k - state.puck_temperature_k);
        const double heat_loss_w =
            coeff.ambient_heat_loss_w_k * (state.puck_temperature_k - coeff.ambient_temperature_k);
        const double dT_dt = (heat_in_w - heat_loss_w) / thermal_capacity_j_k;
        state.puck_temperature_k += dT_dt * dt;

        if (std::abs(dT_dt * dt) > 5.0) {
            warn.once(result.warnings, "TEMPERATURE_STEP_LARGE",
                      "puck temperature moved more than 5 K in one step; reduce dt_s", state.time_s,
                      WarningSeverity::soft);
        }
        state.puck_temperature_k =
            std::clamp(state.puck_temperature_k, water_->min_temperature_k(),
                       water_->max_temperature_k());
        diag.min_puck_temperature_k = std::min(diag.min_puck_temperature_k, state.puck_temperature_k);
        diag.max_puck_temperature_k = std::max(diag.max_puck_temperature_k, state.puck_temperature_k);

        // 9.2 step 6: extraction into the pore liquid (8.2), then transport of
        // dissolved solids into the cup (8.4).
        const double k_ext = extraction_rate_coefficient(state, recipe, coeff, d.flow.flow_m3_s);
        double extracted_kg = k_ext * state.remaining_extractable_solids_kg * dt;
        extracted_kg = std::clamp(extracted_kg, 0.0, state.remaining_extractable_solids_kg);
        state.remaining_extractable_solids_kg -= extracted_kg;
        state.dissolved_solids_kg += extracted_kg;
        state.retained_water_kg += extracted_kg;  // dissolved mass joins the pore liquid

        // Liquid leaves only once the pores are full, which is what delays
        // beverage production during pre-infusion (6.4).
        const double capacity_kg = std::max(d.pore_capacity_kg, kMassEpsilon);
        double out_kg = std::max(state.retained_water_kg - capacity_kg, 0.0);
        out_kg = std::min(out_kg, state.retained_water_kg);
        if (out_kg > 0.0) {
            const double c_pore = state.dissolved_solids_kg /
                                  std::max(state.retained_water_kg, kMassEpsilon);
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
                break;
            }
            ++diag.clamp_count;
        }
        state.liquid_saturation = std::clamp(state.liquid_saturation, 0.0, 1.0);

        // 9.2 step 7.
        state.time_s = static_cast<double>(step + 1) * dt;  // accumulate from the step index
        diag.step_count = step + 1;
    }

    // --- Invariants and residuals (9.3)
    const double solids_total_kg =
        state.dissolved_solids_kg + state.dissolved_solids_in_cup_kg;
    diag.water_mass_residual_kg = state.cumulative_water_in_kg + solids_total_kg -
                                  (state.retained_water_kg + state.beverage_mass_kg);
    diag.solids_mass_residual_kg =
        (initial_extractable_kg - state.remaining_extractable_solids_kg) - solids_total_kg;
    if (diag.min_permeability_m2 == std::numeric_limits<double>::max()) {
        diag.min_permeability_m2 = 0.0;
    }

    // Final sample so the series always ends at the termination time.
    if (result.samples.empty() || result.samples.back().time_s < state.time_s) {
        result.samples.push_back(
            make_sample(state, boundaries_at_sample, derived_at_sample, recipe));
    }

    ShotSummary& summary = result.summary;
    summary.termination = termination;
    summary.elapsed_time_s = state.time_s;
    summary.target_mass_reached = termination == TerminationReason::target_mass_reached;
    summary.beverage_mass_kg = state.beverage_mass_kg;
    summary.tds_fraction = tds_of(state);
    summary.extraction_yield_fraction = yield_of(state, recipe);
    summary.brew_ratio =
        recipe.dose_kg > kMassEpsilon ? state.beverage_mass_kg / recipe.dose_kg : 0.0;
    summary.peak_flow_m3_s = diag.max_flow_m3_s;
    summary.average_flow_m3_s = 0.0;
    if (state.time_s > 0.0) {
        double total_volume_m3 = 0.0;
        for (const auto& sample : result.samples) total_volume_m3 += sample.flow_m3_s;
        summary.average_flow_m3_s =
            result.samples.empty() ? 0.0 : total_volume_m3 / static_cast<double>(result.samples.size());
    }
    summary.warning_count = static_cast<int>(result.warnings.size());

    if (summary.beverage_mass_kg <= kMassEpsilon) {
        result.warnings.push_back({"NO_BEVERAGE_PRODUCED",
                                   "the puck never saturated enough to release beverage",
                                   state.time_s, WarningSeverity::hard});
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
