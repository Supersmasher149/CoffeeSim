#include "espressolab/simulator.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include "espressolab/extraction.hpp"
#include "espressolab/flavor.hpp"
#include "espressolab/puck.hpp"
#include "espressolab/units.hpp"
#include "espressolab/version.hpp"

namespace espressolab {
namespace {

constexpr double kMassEpsilon = 1.0e-12;
// Slack for comparing accumulated sample times against step times.
constexpr double kTimeEpsilonS = 1.0e-9;
constexpr double kSaturationTolerance = 1.0e-6;
// A puck temperature change larger than this in one step means dt_s is too
// coarse for the heat balance; the solver warns rather than refusing.
constexpr double kLargeTemperatureStepK = 5.0;
// dt_s and maximum_time_s each validate individually (finite, positive;
// 10-60 s), but nothing bounded their ratio. A syntactically valid dt_s of
// 1e-7 s with maximum_time_s at its 60 s ceiling implies ~6e8 fixed steps,
// and neither the CLI nor the REST server passes a cancellation callback
// into Simulator::run, so that step loop was an unbounded, uncancellable
// hang reachable from a single request. This caps the implied step count
// comfortably above every dt_s used anywhere in the repo today (smallest is
// 0.005 s, tests/integration/test_convergence.cpp) while keeping worst-case
// blocking time on the synchronous REST path bounded.
constexpr double kMaxSolverSteps = 2'000'000.0;

struct Boundaries {
    double pressure_pa = 0.0;
    double delta_p_pa = 0.0;
    double inlet_temperature_k = 0.0;
};

// The per-cell closure values evaluate_regions() derives from one cell's state.
struct CellDerived {
    double permeability_m2 = 0.0;
    double pore_capacity_kg = 0.0;
    double water_heat_capacity_j_kg_k = 0.0;
};

struct Derived {
    PuckGeometry geometry;
    FlowSolution flow;
    double viscosity_pa_s = 0.0;
    double inlet_density_kg_m3 = 0.0;
    double water_heat_capacity_j_kg_k = 0.0;
    // Series-equivalent for the region, so the reported chart field keeps its
    // Level 2 meaning; the per-cell values below are what the solver steps.
    double permeability_m2 = 0.0;
    double pore_capacity_kg = 0.0;
    double cell_depth_m = 0.0;
    std::vector<CellDerived> cells;
};

// Sensory overlay only (docs/model.md). One pool per solute class, tracking
// the same solids the cell's physical stores already account for -- these
// divide that mass, they never add to it. Empty unless the recipe carries a
// bean. Only the tracer_* functions and partition_classes() write them, and
// they read nothing but physics values, so the overlay cannot feed back.
struct SoluteClassPools {
    std::vector<double> remaining_kg;
    std::vector<double> dissolved_kg;
};

// One axial finite-volume cell. Level 2 is this vector with a single entry.
struct CellState {
    double temperature_k = 0.0;
    double liquid_saturation = 0.0;
    double retained_water_kg = 0.0;
    double dissolved_solids_kg = 0.0;
    double remaining_extractable_solids_kg = 0.0;
    // One extractable pool per PSD bin, when the recipe supplies a
    // distribution. Empty on the scalar path, where
    // remaining_extractable_solids_kg is the only store. When it is populated
    // that scalar stays authoritative -- it is recomputed as the sum of the
    // bins each step, so aggregation, the mass balances and the sample series
    // all keep reading one number and need no knowledge of the bins.
    std::vector<double> bin_remaining_kg;
    SoluteClassPools classes;
};

struct RegionState {
    // The rolled-up view of the cells, carrying the region's cup totals. Every
    // consumer downstream of the step loop reads this rather than the cells.
    ShotState shot;
    std::vector<CellState> cells;
    double integrated_flow_m3 = 0.0;
};

// Sensible heat capacity of a coffee mass plus the water it holds. The water
// term is floored so a dry puck still has a finite, non-zero capacity.
double thermal_capacity_j_k(double coffee_kg, const ModelCoefficients& coeff,
                            double water_kg, double water_heat_capacity_j_kg_k) {
    return coffee_kg * coeff.coffee_heat_capacity_j_kg_k +
           std::max(water_kg, kMassEpsilon) * water_heat_capacity_j_kg_k;
}

bool all_finite(const CellState& cell) {
    for (double bin_kg : cell.bin_remaining_kg) {
        if (!std::isfinite(bin_kg)) return false;
    }
    for (double class_kg : cell.classes.remaining_kg) {
        if (!std::isfinite(class_kg)) return false;
    }
    for (double class_kg : cell.classes.dissolved_kg) {
        if (!std::isfinite(class_kg)) return false;
    }
    return std::isfinite(cell.temperature_k) && std::isfinite(cell.liquid_saturation) &&
           std::isfinite(cell.retained_water_kg) && std::isfinite(cell.dissolved_solids_kg) &&
           std::isfinite(cell.remaining_extractable_solids_kg);
}

// Collapse the axial column back onto the region state the rest of the solver
// reads. Totals are sums; saturation is the pore-capacity weighted mean, which
// total retained over total capacity gives directly; temperature is weighted by
// each cell's thermal capacity.
void roll_up(RegionState& region, const Derived& derived, double region_dose_kg,
             const ModelCoefficients& coeff) {
    ShotState& shot = region.shot;
    shot.remaining_extractable_solids_kg = 0.0;
    shot.dissolved_solids_kg = 0.0;
    shot.retained_water_kg = 0.0;

    if (region.cells.size() == 1) {
        const CellState& cell = region.cells.front();
        shot.remaining_extractable_solids_kg = cell.remaining_extractable_solids_kg;
        shot.dissolved_solids_kg = cell.dissolved_solids_kg;
        shot.retained_water_kg = cell.retained_water_kg;
        shot.puck_temperature_k = cell.temperature_k;
        shot.liquid_saturation = cell.liquid_saturation;
        return;
    }

    const double cell_dose_kg = region_dose_kg / static_cast<double>(region.cells.size());
    double thermal_capacity_sum = 0.0;
    double weighted_temperature = 0.0;
    double capacity_sum = 0.0;
    for (std::size_t i = 0; i < region.cells.size(); ++i) {
        const CellState& cell = region.cells[i];
        shot.remaining_extractable_solids_kg += cell.remaining_extractable_solids_kg;
        shot.dissolved_solids_kg += cell.dissolved_solids_kg;
        shot.retained_water_kg += cell.retained_water_kg;

        const double thermal_capacity =
            thermal_capacity_j_k(cell_dose_kg, coeff, cell.retained_water_kg,
                                 derived.cells[i].water_heat_capacity_j_kg_k);
        thermal_capacity_sum += thermal_capacity;
        weighted_temperature += cell.temperature_k * thermal_capacity;
        capacity_sum += derived.cells[i].pore_capacity_kg;
    }
    shot.puck_temperature_k = thermal_capacity_sum > kMassEpsilon
                                  ? weighted_temperature / thermal_capacity_sum
                                  : region.cells.front().temperature_k;
    shot.liquid_saturation =
        capacity_sum > kMassEpsilon ? shot.retained_water_kg / capacity_sum : 0.0;
}

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
    for (double class_kg : state.class_in_cup_kg) {
        if (!std::isfinite(class_kg)) return false;
    }
    return std::isfinite(state.time_s) && std::isfinite(state.puck_temperature_k) &&
           std::isfinite(state.permeability_m2) && std::isfinite(state.liquid_saturation) &&
           std::isfinite(state.remaining_extractable_solids_kg) &&
           std::isfinite(state.dissolved_solids_kg) && std::isfinite(state.beverage_mass_kg) &&
           std::isfinite(state.cumulative_water_in_kg) && std::isfinite(state.retained_water_kg) &&
           std::isfinite(state.dissolved_solids_in_cup_kg);
}

bool all_finite(const std::vector<RegionState>& regions) {
    return std::all_of(regions.begin(), regions.end(), [](const RegionState& region) {
        return all_finite(region.shot) && std::isfinite(region.integrated_flow_m3) &&
               std::all_of(region.cells.begin(), region.cells.end(),
                           [](const CellState& cell) { return all_finite(cell); });
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
    if (!lower.class_in_cup_kg.empty()) {
        state.class_in_cup_kg.resize(lower.class_in_cup_kg.size());
        for (std::size_t k = 0; k < lower.class_in_cup_kg.size(); ++k) {
            state.class_in_cup_kg[k] =
                interpolate(lower.class_in_cup_kg[k], upper.class_in_cup_kg[k]);
        }
    }
    return state;
}

RegionState interpolate_region(const RegionState& lower, const RegionState& upper, double time_s) {
    const double span = upper.shot.time_s - lower.shot.time_s;
    const double fraction = span > 0.0 ? (time_s - lower.shot.time_s) / span : 0.0;
    RegionState state;
    state.shot = interpolate_state(lower.shot, upper.shot, time_s);
    state.integrated_flow_m3 =
        lower.integrated_flow_m3 + fraction * (upper.integrated_flow_m3 - lower.integrated_flow_m3);
    state.cells.reserve(lower.cells.size());
    for (std::size_t i = 0; i < lower.cells.size(); ++i) {
        const auto blend = [&](double a, double b) { return a + fraction * (b - a); };
        CellState cell;
        cell.temperature_k = blend(lower.cells[i].temperature_k, upper.cells[i].temperature_k);
        cell.liquid_saturation =
            blend(lower.cells[i].liquid_saturation, upper.cells[i].liquid_saturation);
        cell.retained_water_kg =
            blend(lower.cells[i].retained_water_kg, upper.cells[i].retained_water_kg);
        cell.dissolved_solids_kg =
            blend(lower.cells[i].dissolved_solids_kg, upper.cells[i].dissolved_solids_kg);
        cell.remaining_extractable_solids_kg =
            blend(lower.cells[i].remaining_extractable_solids_kg,
                  upper.cells[i].remaining_extractable_solids_kg);
        state.cells.push_back(cell);
    }
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
    if (!regions.front().shot.class_in_cup_kg.empty()) {
        aggregate.class_in_cup_kg.assign(regions.front().shot.class_in_cup_kg.size(), 0.0);
    }
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const ShotState& state = regions[i].shot;
        const Derived& region = derived[i];
        aggregate.remaining_extractable_solids_kg += state.remaining_extractable_solids_kg;
        aggregate.dissolved_solids_kg += state.dissolved_solids_kg;
        aggregate.beverage_mass_kg += state.beverage_mass_kg;
        aggregate.cumulative_water_in_kg += state.cumulative_water_in_kg;
        aggregate.retained_water_kg += state.retained_water_kg;
        aggregate.dissolved_solids_in_cup_kg += state.dissolved_solids_in_cup_kg;
        for (std::size_t k = 0; k < aggregate.class_in_cup_kg.size(); ++k) {
            aggregate.class_in_cup_kg[k] += state.class_in_cup_kg[k];
        }

        const double thermal_capacity = thermal_capacity_j_k(
            recipe.dose_kg * recipe.parallel_regions[i].area_fraction, coeff,
            state.retained_water_kg, region.water_heat_capacity_j_kg_k);
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

void validate_inputs(const Recipe& recipe, const ModelCoefficients& coeff,
                     const SimulationConfig& config) {
    ValidationResult validation = recipe.validate();
    validation.merge(coeff.validate());
    // Audit F1: dt_s/sample_interval_s were only checked with `<= 0.0`, which
    // NaN and infinity pass (NaN compares false against everything), letting
    // non-finite controls reach the stepping loop. require_positive() rejects
    // non-finite values first and reports the field that actually failed.
    require_positive(validation, config.dt_s, "config.dt_s");
    require_positive(validation, config.sample_interval_s, "config.sample_interval_s");
    // Only check the dt_s/maximum_time_s ratio once both are known
    // individually valid -- dividing by an already-rejected dt_s (e.g. 0)
    // would just add a confusing second issue on top of the real one.
    if (validation.ok() && recipe.maximum_time_s / config.dt_s > kMaxSolverSteps) {
        validation.add("STEP_COUNT_EXCEEDS_LIMIT",
                       "config.dt_s is too small for recipe.maximum_time_s: implies more than " +
                           std::to_string(static_cast<long long>(kMaxSolverSteps)) +
                           " fixed steps",
                       "config.dt_s");
    }
    if (!validation.ok()) throw InvalidInputError(validation);
}

std::vector<RegionState> initialize_regions(const Recipe& recipe,
                                            const ModelCoefficients& coeff) {
    std::vector<RegionState> regions(recipe.parallel_regions.size());
    const std::size_t cell_count = static_cast<std::size_t>(recipe.axial_cells);
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const double region_extractable_kg = recipe.dose_kg *
                                             recipe.parallel_regions[i].area_fraction *
                                             coeff.extractable_solids_fraction;
        regions[i].shot.puck_temperature_k = coeff.initial_puck_temperature_k;
        regions[i].shot.remaining_extractable_solids_kg = region_extractable_kg;
        if (recipe.bean.has_value()) {
            regions[i].shot.class_in_cup_kg.assign(kSoluteClassCount, 0.0);
        }
        regions[i].cells.assign(cell_count, CellState{});
        for (CellState& cell : regions[i].cells) {
            cell.temperature_k = coeff.initial_puck_temperature_k;
            // The dose, and so the extractable solids, divides evenly down the
            // column; the sum over cells is the region's share exactly.
            cell.remaining_extractable_solids_kg =
                region_extractable_kg / static_cast<double>(cell_count);
            if (recipe.grind.has_value()) {
                // Extractable mass splits across the size classes in proportion
                // to the mass each class holds, so the bins sum to the cell's
                // share exactly and the scalar total is unchanged at t = 0.
                const auto& bins = recipe.grind->bins;
                cell.bin_remaining_kg.reserve(bins.size());
                for (const GrindBin& bin : bins) {
                    cell.bin_remaining_kg.push_back(cell.remaining_extractable_solids_kg *
                                                    bin.mass_fraction);
                }
            }
            if (recipe.bean.has_value()) {
                // The same split, along a second and independent axis: solute
                // class rather than particle size. BeanProfile::validate()
                // holds the fractions to a sum of 1, so the classes sum to the
                // cell's share exactly and the scalar total is untouched at
                // t = 0 -- exactly as the bins above are.
                cell.classes.remaining_kg.reserve(kSoluteClassCount);
                for (const SoluteClassShare& share : recipe.bean->classes) {
                    cell.classes.remaining_kg.push_back(cell.remaining_extractable_solids_kg *
                                                        share.mass_fraction);
                }
                cell.classes.dissolved_kg.assign(kSoluteClassCount, 0.0);
            }
        }
    }
    return regions;
}

std::pair<Boundaries, std::vector<Derived>> evaluate_regions(
    const std::vector<RegionState>& states, const Recipe& recipe,
    const ModelCoefficients& coeff, const WaterProperties& water, double area_m2) {
    Boundaries boundaries;
    boundaries.pressure_pa = recipe.pressure_pa.sample(states.front().shot.time_s);
    boundaries.inlet_temperature_k =
        recipe.inlet_temperature_k.sample(states.front().shot.time_s);
    boundaries.delta_p_pa = boundaries.pressure_pa - coeff.outlet_pressure_pa;

    std::vector<Derived> derived;
    derived.reserve(states.size());
    for (std::size_t i = 0; i < states.size(); ++i) {
        const ShotState& state = states[i].shot;
        const ParallelRegion& region = recipe.parallel_regions[i];
        const std::vector<CellState>& cells = states[i].cells;
        const double cell_count = static_cast<double>(cells.size());

        Derived d;
        d.viscosity_pa_s = water.viscosity_pa_s(state.puck_temperature_k);
        d.inlet_density_kg_m3 = water.density_kg_m3(boundaries.inlet_temperature_k);
        d.water_heat_capacity_j_kg_k = water.heat_capacity_j_kg_k(state.puck_temperature_k);
        d.geometry = compress_puck(recipe, coeff, boundaries.delta_p_pa);

        const double region_area_m2 = area_m2 * region.area_fraction;
        const double k0 = kozeny_carman_permeability(recipe.particle_diameter_m,
                                                      d.geometry.porosity,
                                                      coeff.kozeny_constant);
        const double shape =
            k0 * distribution_factor(recipe.particle_spread_factor, coeff.distribution_factor_floor) *
            region.permeability_multiplier;
        // The compressed depth is split evenly; every cell shares the
        // region's porosity and cross-section and differs only by state.
        d.cell_depth_m = d.geometry.depth_m / cell_count;

        std::vector<AxialCell> column;
        column.reserve(cells.size());
        d.cells.reserve(cells.size());
        for (const CellState& cell : cells) {
            const double permeability =
                shape * wetting_factor(cell.liquid_saturation, coeff.dry_permeability_multiplier);
            d.cells.push_back({permeability,
                               region_area_m2 * d.cell_depth_m * d.geometry.porosity *
                                   water.density_kg_m3(cell.temperature_k),
                               water.heat_capacity_j_kg_k(cell.temperature_k)});
            column.push_back({permeability, water.viscosity_pa_s(cell.temperature_k),
                              d.cell_depth_m});
        }

        d.flow = darcy_flow_series(column, region_area_m2, boundaries.delta_p_pa,
                                   coeff.maximum_flow_m3_s);
        d.pore_capacity_kg = 0.0;
        for (const CellDerived& cell : d.cells) d.pore_capacity_kg += cell.pore_capacity_kg;

        // A single cell reports its own permeability; a column reports the
        // series-equivalent at the region's rolled-up viscosity, so the
        // chart field stays comparable across cell counts.
        d.permeability_m2 =
            cells.size() == 1
                ? d.cells.front().permeability_m2
                : (d.flow.resistance_pa_s_m3 > 0.0
                       ? (d.viscosity_pa_s * d.geometry.depth_m) /
                             (d.flow.resistance_pa_s_m3 * region_area_m2)
                       : 0.0);
        derived.push_back(std::move(d));
    }
    return {boundaries, derived};
}

// What one step leaves the solver: the read-only inputs every stage shares,
// and the sinks it reports into.
struct StepContext {
    const Recipe& recipe;
    const ModelCoefficients& coeff;
    const SimulationConfig& config;
    const WaterProperties& water;
    double dt;
};

struct StepOutputs {
    ShotResult& result;
    WarningLog& warn;
    ShotDiagnostics& diag;
};

// The liquid one cell passes to the next in the axial sweep: the inlet water
// for cell 0, and what leaves the last cell is beverage. class_kg is the
// sensory overlay's breakdown of solids_kg and stays empty without a bean.
struct Parcel {
    double mass_kg = 0.0;
    double solids_kg = 0.0;
    double temperature_k = 0.0;
    std::vector<double> class_kg;
};

void receive(CellState& cell, const Parcel& in) {
    cell.retained_water_kg += in.mass_kg;
    cell.dissolved_solids_kg += in.solids_kg;
}

// Overlay counterpart of receive(): the incoming class breakdown joins the
// cell's dissolved pools.
void tracer_receive(SoluteClassPools& pools, const std::vector<double>& in_class_kg) {
    for (std::size_t k = 0; k < pools.dissolved_kg.size(); ++k) {
        pools.dissolved_kg[k] += in_class_kg[k];
    }
}

// Rate of temperature change of one cell: advection from the incoming
// parcel against a share of the region's ambient loss.
double heat_rate_k_s(const CellState& cell, const CellDerived& cd, const Parcel& in,
                 double cell_dose_kg, double cell_heat_loss_w_k, const StepContext& ctx) {
    const double capacity_j_k = thermal_capacity_j_k(cell_dose_kg, ctx.coeff,
                                                     cell.retained_water_kg,
                                                     cd.water_heat_capacity_j_kg_k);
    const double mass_flow_kg_s = in.mass_kg / ctx.dt;
    const double heat_in_w = mass_flow_kg_s * cd.water_heat_capacity_j_kg_k *
                             (in.temperature_k - cell.temperature_k);
    const double heat_loss_w =
        cell_heat_loss_w_k * (cell.temperature_k - ctx.coeff.ambient_temperature_k);
    return (heat_in_w - heat_loss_w) / capacity_j_k;
}

// Moves solids from the cell's extractable store(s) into its pore liquid and
// returns the mass moved. Reads this cell's own temperature and saturation.
double extract_step(CellState& cell, double flow_m3_s, const StepContext& ctx) {
    ShotState cell_view;
    cell_view.puck_temperature_k = cell.temperature_k;
    cell_view.liquid_saturation = cell.liquid_saturation;
    double extracted_kg = 0.0;
    if (cell.bin_remaining_kg.empty()) {
        const double k_ext =
            extraction_rate_coefficient(cell_view, ctx.recipe, ctx.coeff, flow_m3_s);
        extracted_kg = k_ext * cell.remaining_extractable_solids_kg * ctx.dt;
        extracted_kg = std::clamp(extracted_kg, 0.0, cell.remaining_extractable_solids_kg);
        cell.remaining_extractable_solids_kg -= extracted_kg;
    } else {
        // Size-resolved: each class extracts at its own rate, so the
        // fines exhaust while the coarse mode is still producing. That
        // ordering is the whole reason for carrying a distribution --
        // a single mean diameter cannot express it.
        const std::vector<GrindBin>& bins = ctx.recipe.grind->bins;
        double remaining_total_kg = 0.0;
        for (std::size_t b = 0; b < cell.bin_remaining_kg.size(); ++b) {
            const double k_ext = extraction_rate_coefficient_at(cell_view, ctx.coeff, flow_m3_s,
                                                                bins[b].diameter_m);
            double bin_extracted_kg = k_ext * cell.bin_remaining_kg[b] * ctx.dt;
            bin_extracted_kg = std::clamp(bin_extracted_kg, 0.0, cell.bin_remaining_kg[b]);
            cell.bin_remaining_kg[b] -= bin_extracted_kg;
            extracted_kg += bin_extracted_kg;
            remaining_total_kg += cell.bin_remaining_kg[b];
        }
        cell.remaining_extractable_solids_kg = remaining_total_kg;
    }
    cell.dissolved_solids_kg += extracted_kg;
    cell.retained_water_kg += extracted_kg;
    return extracted_kg;
}

// The sensory overlay. It reads extracted_kg and writes only the class pools:
// nothing in the physics observes them, so the lumped arithmetic in
// extract_step() is the authority and stays untouched. Returns how many
// classes hit their floor.
//
// Each class takes a share of the mass the solver already extracted, in
// proportion to how much of it is left times how readily it leaves. The
// shares sum to extracted_kg by construction rather than by a corrective
// renormalisation, so total dissolved solids cannot drift. Deliberately no
// second call to extraction_rate_coefficient(): only the ratio between
// classes matters, which also means the scalar and PSD branches need no
// separate treatment here.
int partition_classes(SoluteClassPools& pools, const BeanProfile& bean, double extracted_kg) {
    int clamp_count = 0;
    double propensity_sum = 0.0;
    std::array<double, kSoluteClassCount> propensity{};
    for (std::size_t k = 0; k < kSoluteClassCount; ++k) {
        propensity[k] = bean.classes[k].relative_rate * pools.remaining_kg[k];
        propensity_sum += propensity[k];
    }
    if (propensity_sum <= 0.0) return clamp_count;

    double placed_kg = 0.0;
    for (std::size_t k = 0; k < kSoluteClassCount; ++k) {
        double take_kg = extracted_kg * propensity[k] / propensity_sum;
        if (take_kg > pools.remaining_kg[k]) {
            take_kg = pools.remaining_kg[k];
            ++clamp_count;
        }
        pools.remaining_kg[k] -= take_kg;
        pools.dissolved_kg[k] += take_kg;
        placed_kg += take_kg;
    }
    // A class that hit its floor leaves a shortfall. Spread it over the
    // classes that still have mass so the overlay keeps accounting for
    // exactly extracted_kg; if none do, the residual is reported rather than
    // silently dropped.
    const double shortfall_kg = extracted_kg - placed_kg;
    if (shortfall_kg > 0.0) {
        double available_kg = 0.0;
        for (double remaining_kg : pools.remaining_kg) available_kg += remaining_kg;
        if (available_kg > 0.0) {
            const double fill = std::min(shortfall_kg / available_kg, 1.0);
            for (std::size_t k = 0; k < kSoluteClassCount; ++k) {
                const double extra_kg = pools.remaining_kg[k] * fill;
                pools.remaining_kg[k] -= extra_kg;
                pools.dissolved_kg[k] += extra_kg;
            }
        }
    }
    return clamp_count;
}

// Releases whatever pore liquid exceeds the cell's capacity, at the cell's
// pore concentration, overwriting `out`'s physical fields with it for the next
// cell down. Returns the pore solids held before the release, which is all the
// overlay needs to follow it.
double drain(CellState& cell, double capacity_kg, Parcel& out) {
    const double solids_before_out_kg = cell.dissolved_solids_kg;
    double out_kg = std::max(cell.retained_water_kg - capacity_kg, 0.0);
    out_kg = std::min(out_kg, cell.retained_water_kg);
    double solids_out_kg = 0.0;
    if (out_kg > 0.0) {
        const double c_pore =
            cell.dissolved_solids_kg / std::max(cell.retained_water_kg, kMassEpsilon);
        solids_out_kg = std::min(out_kg * c_pore, cell.dissolved_solids_kg);
        cell.dissolved_solids_kg -= solids_out_kg;
        cell.retained_water_kg -= out_kg;
    }
    out.mass_kg = out_kg;
    out.solids_kg = solids_out_kg;
    out.temperature_k = cell.temperature_k;
    return solids_before_out_kg;
}

// Overlay counterpart of drain(). The tracer travels with the pore liquid at
// exactly the fraction of solids the solver actually moved, so the overlay
// invents no second transport rule of its own.
void tracer_drain(SoluteClassPools& pools, double solids_before_out_kg, double solids_out_kg,
                  std::vector<double>& out_class_kg) {
    const double moved_fraction =
        solids_before_out_kg > 0.0 ? solids_out_kg / solids_before_out_kg : 0.0;
    for (std::size_t k = 0; k < pools.dissolved_kg.size(); ++k) {
        const double moved_kg = pools.dissolved_kg[k] * moved_fraction;
        pools.dissolved_kg[k] -= moved_kg;
        out_class_kg[k] = moved_kg;
    }
}

// Advances every region one step. Returns a termination reason only when a
// strict invariant stops the run; otherwise the step completed.
std::optional<TerminationReason> advance_regions(std::vector<RegionState>& regions,
                                                 const std::vector<Derived>& derived,
                                                 const Boundaries& boundaries,
                                                 const StepContext& ctx, long long step,
                                                 StepOutputs& out) {
    const Recipe& recipe = ctx.recipe;
    const ModelCoefficients& coeff = ctx.coeff;
    const double dt = ctx.dt;
    int flavor_clamp_count = 0;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        RegionState& region = regions[i];
        ShotState& state = region.shot;
        const Derived& d = derived[i];
        const double cells_in_region = static_cast<double>(region.cells.size());
        const double region_dose_kg = recipe.dose_kg * recipe.parallel_regions[i].area_fraction;
        const double cell_dose_kg = region_dose_kg / cells_in_region;
        // Ambient loss is a property of the region, not of the grid, so it
        // is divided across cells rather than applied once per cell.
        const double cell_heat_loss_w_k = coeff.ambient_heat_loss_w_k / cells_in_region;

        const double water_in_kg = d.flow.flow_m3_s * d.inlet_density_kg_m3 * dt;
        region.integrated_flow_m3 += d.flow.flow_m3_s * dt;
        state.cumulative_water_in_kg += water_in_kg;

        // The axial sweep: cell 0 takes the inlet, every cell below takes
        // what the cell above it released, at that cell's temperature and
        // pore concentration. What leaves the last cell is beverage.
        Parcel parcel;
        parcel.mass_kg = water_in_kg;
        parcel.temperature_k = boundaries.inlet_temperature_k;
        const bool traced = recipe.bean.has_value();
        if (traced) parcel.class_kg.assign(kSoluteClassCount, 0.0);

        for (std::size_t c = 0; c < region.cells.size(); ++c) {
            CellState& cell = region.cells[c];
            const CellDerived& cd = d.cells[c];
            receive(cell, parcel);
            if (traced) tracer_receive(cell.classes, parcel.class_kg);

            // Kept as one multiply-add: the compiler may fuse it, and splitting
            // the product out would change the last ulp and the result hash.
            const double dT_dt = heat_rate_k_s(cell, cd, parcel, cell_dose_kg, cell_heat_loss_w_k, ctx);
            cell.temperature_k += dT_dt * dt;
            if (std::abs(dT_dt * dt) > kLargeTemperatureStepK) {
                out.warn.once(out.result.warnings, "TEMPERATURE_STEP_LARGE",
                              "puck temperature moved more than 5 K in one step; reduce dt_s",
                              state.time_s, WarningSeverity::soft);
            }
            cell.temperature_k = std::clamp(cell.temperature_k, ctx.water.min_temperature_k(),
                                            ctx.water.max_temperature_k());
            out.diag.min_puck_temperature_k =
                std::min(out.diag.min_puck_temperature_k, cell.temperature_k);
            out.diag.max_puck_temperature_k =
                std::max(out.diag.max_puck_temperature_k, cell.temperature_k);

            const double extracted_kg = extract_step(cell, d.flow.flow_m3_s, ctx);
            if (traced) {
                flavor_clamp_count += partition_classes(cell.classes, *recipe.bean, extracted_kg);
            }

            const double capacity_kg = std::max(cd.pore_capacity_kg, kMassEpsilon);
            const double solids_before_out_kg = drain(cell, capacity_kg, parcel);
            if (traced) {
                tracer_drain(cell.classes, solids_before_out_kg, parcel.solids_kg,
                             parcel.class_kg);
            }

            cell.liquid_saturation = cell.retained_water_kg / capacity_kg;
            if (cell.liquid_saturation > 1.0 + kSaturationTolerance ||
                cell.liquid_saturation < -kSaturationTolerance) {
                if (ctx.config.strict_invariants) {
                    out.result.warnings.push_back({"SATURATION_INVARIANT",
                                                   "liquid saturation left [0, 1] beyond tolerance",
                                                   state.time_s, WarningSeverity::hard});
                    return TerminationReason::invalid_state;
                }
                ++out.diag.clamp_count;
            }
            cell.liquid_saturation = std::clamp(cell.liquid_saturation, 0.0, 1.0);
        }

        // Whatever the last cell released has left the puck.
        state.beverage_mass_kg += parcel.mass_kg;
        state.dissolved_solids_in_cup_kg += parcel.solids_kg;
        for (std::size_t k = 0; k < state.class_in_cup_kg.size(); ++k) {
            state.class_in_cup_kg[k] += parcel.class_kg[k];
        }
        roll_up(region, d, region_dose_kg, coeff);
    }

    for (RegionState& region : regions) {
        region.shot.time_s = static_cast<double>(step + 1) * dt;
    }
    out.diag.step_count = step + 1;
    if (out.result.flavor.has_value()) {
        out.result.flavor->summary.class_clamp_count += flavor_clamp_count;
    }
    return std::nullopt;
}

// The flavour view of one cup state: what fraction of the solids in the cup
// belongs to each solute class, and what that composition tastes like at this
// strength. Pure post-processing -- it reads the cup and writes nothing back.
FlavorSample make_flavor_sample(const ShotState& state, const BeanProfile& bean,
                                double tds_fraction) {
    FlavorSample sample;
    sample.time_s = state.time_s;
    double total_kg = 0.0;
    for (double class_kg : state.class_in_cup_kg) total_kg += class_kg;
    if (total_kg > kMassEpsilon) {
        for (std::size_t k = 0; k < kSoluteClassCount; ++k) {
            sample.composition[k] = state.class_in_cup_kg[k] / total_kg;
        }
        sample.intensity = axis_intensities(sample.composition, bean.axis_weights, tds_fraction);
    }
    // Before the first drops there is nothing in the cup: composition and every
    // intensity stay zero rather than reporting the flavour of no coffee.
    return sample;
}

void append_sample(ShotResult& result, const ShotState& state, const Boundaries& boundaries,
                   double flow_m3_s, const Recipe& recipe) {
    const ShotSample sample = make_sample(state, boundaries, flow_m3_s, recipe);
    if (result.flavor.has_value() && recipe.bean.has_value()) {
        result.flavor->series.push_back(
            make_flavor_sample(state, *recipe.bean, sample.tds_fraction));
    }
    result.samples.push_back(sample);
}

// When the next sample falls due. The next time accumulates by repeated
// addition of the interval; recomputing it as a multiple would move the
// sample times in the last ulp and with them the result hash.
class SampleClock {
public:
    explicit SampleClock(double interval_s) : interval_s_(interval_s) {}

    bool due(double time_s) const { return time_s + kTimeEpsilonS >= next_s_; }
    // advance_regions sets shot.time_s to exactly (step + 1) * dt, so a step
    // starting at time_s crosses the next sample iff this holds -- letting the
    // (potentially large, per-cell) pre-step snapshot be skipped otherwise.
    bool crossed_by_step(double time_s, double dt) const {
        return next_s_ <= time_s + dt + kTimeEpsilonS;
    }
    bool passed_by(double time_s) const { return next_s_ <= time_s + kTimeEpsilonS; }
    double next_s() const { return next_s_; }
    void advance() { next_s_ += interval_s_; }

private:
    double interval_s_;
    double next_s_ = 0.0;
};

// Records every sample time the last step passed, each interpolated between
// the pre-step snapshot and the current regions and re-evaluated so derived
// fields match the interpolated state.
void append_interpolated_samples(SampleClock& clock, const std::vector<RegionState>& before,
                                 const std::vector<RegionState>& after, const StepContext& ctx,
                                 double area_m2, ShotResult& result) {
    while (clock.passed_by(after.front().shot.time_s)) {
        std::vector<RegionState> sampled_regions;
        sampled_regions.reserve(after.size());
        for (std::size_t i = 0; i < after.size(); ++i) {
            sampled_regions.push_back(interpolate_region(before[i], after[i], clock.next_s()));
        }
        const auto [sampled_boundaries, sampled_derived] =
            evaluate_regions(sampled_regions, ctx.recipe, ctx.coeff, ctx.water, area_m2);
        for (std::size_t i = 0; i < sampled_regions.size(); ++i) {
            sampled_regions[i].shot.permeability_m2 = sampled_derived[i].permeability_m2;
        }
        const ShotState sampled =
            aggregate_state(sampled_regions, sampled_derived, ctx.recipe, ctx.coeff);
        append_sample(result, sampled, sampled_boundaries, total_flow(sampled_derived), ctx.recipe);
        clock.advance();
    }
}

void finalize_result(ShotResult& result, std::vector<RegionState>& regions,
                    const Boundaries& final_boundaries,
                    const std::vector<Derived>& final_derived, const Recipe& recipe,
                    const ModelCoefficients& coeff, const SimulationConfig& config,
                    TerminationReason termination, double initial_extractable_kg,
                    ShotDiagnostics& diag) {
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

    if (result.samples.empty() || result.samples.back().time_s < final_state.time_s - kTimeEpsilonS) {
        append_sample(result, final_state, final_boundaries, total_flow(final_derived), recipe);
    }

    const double integrated_flow_m3 = total_integrated_flow(regions);
    result.regions.reserve(regions.size());
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const ShotState& state = regions[i].shot;
        const ParallelRegion& config_region = recipe.parallel_regions[i];
        const double region_dose_kg = recipe.dose_kg * config_region.area_fraction;
        const double cell_dose_kg =
            region_dose_kg / static_cast<double>(regions[i].cells.size());
        std::vector<AxialCellSummary> cells;
        cells.reserve(regions[i].cells.size());
        for (const CellState& cell : regions[i].cells) {
            const double extracted_kg =
                cell_dose_kg * coeff.extractable_solids_fraction -
                cell.remaining_extractable_solids_kg;
            cells.push_back({cell.liquid_saturation, cell.temperature_k,
                             cell.retained_water_kg > kMassEpsilon
                                 ? cell.dissolved_solids_kg / cell.retained_water_kg
                                 : 0.0,
                             cell_dose_kg > kMassEpsilon ? extracted_kg / cell_dose_kg : 0.0});
        }
        result.regions.push_back({config_region.area_fraction,
                                  config_region.permeability_multiplier,
                                  state.beverage_mass_kg,
                                  integrated_flow_m3 > kMassEpsilon
                                      ? regions[i].integrated_flow_m3 / integrated_flow_m3
                                      : 0.0,
                                  tds_of(state),
                                  yield_of(state, region_dose_kg),
                                  std::move(cells)});
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

    if (result.flavor.has_value() && recipe.bean.has_value()) {
        const FlavorSample final_sample =
            make_flavor_sample(final_state, *recipe.bean, summary.tds_fraction);
        const int clamp_count = result.flavor->summary.class_clamp_count;
        result.flavor->summary = score_against_target(final_sample.composition,
                                                      final_sample.intensity, recipe.bean->target);
        result.flavor->summary.class_clamp_count = clamp_count;
        // The overlay's own mass balance: the class pools must account for the
        // solids in the cup and nothing else. Reported rather than asserted, in
        // the spirit of the residuals above.
        double class_total_kg = 0.0;
        for (double class_kg : final_state.class_in_cup_kg) class_total_kg += class_kg;
        result.flavor->summary.composition_residual =
            class_total_kg - final_state.dissolved_solids_in_cup_kg;
    }

    result.manifest.solver_version = std::string(version::kSolver);
    result.manifest.result_schema_version = std::string(version::kResultSchema);
    result.manifest.coefficient_id = coeff.id;
    result.manifest.coefficient_version = coeff.version;
    result.manifest.dt_s = config.dt_s;
    result.manifest.sample_interval_s = config.sample_interval_s;
}

}  // namespace


Simulator::Simulator() : water_(std::make_shared<TabulatedWaterProperties>()) {}

Simulator::Simulator(std::shared_ptr<const WaterProperties> water) : water_(std::move(water)) {
    if (!water_) water_ = std::make_shared<TabulatedWaterProperties>();
}

ShotResult Simulator::run(const Recipe& recipe, const ModelCoefficients& coeff,
                           const SimulationConfig& config,
                           const CancellationCallback& is_cancelled) const {
    validate_inputs(recipe, coeff, config);

    ShotResult result;
    WarningLog warn;
    if (recipe.bean.has_value()) {
        // Created up front so the step loop and the sample writer can fill it;
        // stays disengaged for every recipe without a bean, which is what keeps
        // the result JSON, the artifacts and the hashes exactly as they were.
        FlavorResult flavor;
        flavor.bean_id = recipe.bean->id;
        flavor.bean_version = recipe.bean->version;
        flavor.flavor_model_version = std::string(version::kFlavorModel);
        result.flavor = flavor;
    }
    std::vector<RegionState> regions = initialize_regions(recipe, coeff);
    const double initial_extractable_kg = recipe.dose_kg * coeff.extractable_solids_fraction;

    ShotDiagnostics& diag = result.diagnostics;
    diag.min_permeability_m2 = std::numeric_limits<double>::max();
    diag.min_puck_temperature_k = coeff.initial_puck_temperature_k;
    diag.max_puck_temperature_k = coeff.initial_puck_temperature_k;

    const double area_m2 = recipe.basket_area_m2();
    const double dt = config.dt_s;
    TerminationReason termination = TerminationReason::not_terminated;
    SampleClock sample_clock(config.sample_interval_s);
    const StepContext step_context{recipe, coeff, config, *water_, dt};
    StepOutputs step_outputs{result, warn, diag};

    for (long long step = 0;; ++step) {
        throw_if_cancelled(is_cancelled);
        const auto [boundaries, derived] =
            evaluate_regions(regions, recipe, coeff, *water_, area_m2);
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
        if (sample_clock.due(aggregate.time_s)) {
            append_sample(result, aggregate, boundaries, flow_m3_s, recipe);
            sample_clock.advance();
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

        const bool crosses_sample_boundary =
            sample_clock.crossed_by_step(regions.front().shot.time_s, dt);
        const std::vector<RegionState> states_before_step =
            crosses_sample_boundary ? regions : std::vector<RegionState>{};
        if (const auto stop = advance_regions(regions, derived, boundaries, step_context, step,
                                              step_outputs)) {
            termination = *stop;
            break;
        }
        if (crosses_sample_boundary) {
            append_interpolated_samples(sample_clock, states_before_step, regions, step_context,
                                        area_m2, result);
        }
    }

    const auto [final_boundaries, final_derived] =
        evaluate_regions(regions, recipe, coeff, *water_, area_m2);
    throw_if_cancelled(is_cancelled);
    finalize_result(result, regions, final_boundaries, final_derived, recipe, coeff, config,
                    termination, initial_extractable_kg, diag);
    return result;
}

}  // namespace espressolab
