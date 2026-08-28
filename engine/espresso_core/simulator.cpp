#include "espressolab/simulator.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

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
    // Series-equivalent for the region, so the reported chart field keeps its
    // Level 2 meaning; the per-cell values below are what the solver steps.
    double permeability_m2 = 0.0;
    double pore_capacity_kg = 0.0;
    double cell_depth_m = 0.0;
    std::vector<double> cell_permeability_m2;
    std::vector<double> cell_pore_capacity_kg;
    std::vector<double> cell_water_heat_capacity_j_kg_k;
};

// One axial finite-volume cell. Level 2 is this vector with a single entry.
struct CellState {
    double temperature_k = 0.0;
    double liquid_saturation = 0.0;
    double retained_water_kg = 0.0;
    double dissolved_solids_kg = 0.0;
    double remaining_extractable_solids_kg = 0.0;
};

struct RegionState {
    // The rolled-up view of the cells, carrying the region's cup totals. Every
    // consumer downstream of the step loop reads this rather than the cells.
    ShotState shot;
    std::vector<CellState> cells;
    double integrated_flow_m3 = 0.0;
};

bool all_finite(const CellState& cell) {
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
            cell_dose_kg * coeff.coffee_heat_capacity_j_kg_k +
            std::max(cell.retained_water_kg, kMassEpsilon) *
                derived.cell_water_heat_capacity_j_kg_k[i];
        thermal_capacity_sum += thermal_capacity;
        weighted_temperature += cell.temperature_k * thermal_capacity;
        capacity_sum += derived.cell_pore_capacity_kg[i];
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
        regions[i].cells.assign(cell_count, CellState{});
        for (CellState& cell : regions[i].cells) {
            cell.temperature_k = coeff.initial_puck_temperature_k;
            // The dose, and so the extractable solids, divides evenly down the
            // column; the sum over cells is the region's share exactly.
            cell.remaining_extractable_solids_kg =
                region_extractable_kg / static_cast<double>(cell_count);
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
        d.cell_permeability_m2.reserve(cells.size());
        d.cell_pore_capacity_kg.reserve(cells.size());
        d.cell_water_heat_capacity_j_kg_k.reserve(cells.size());
        for (const CellState& cell : cells) {
            const double permeability =
                shape * wetting_factor(cell.liquid_saturation, coeff.dry_permeability_multiplier);
            d.cell_permeability_m2.push_back(permeability);
            d.cell_pore_capacity_kg.push_back(region_area_m2 * d.cell_depth_m *
                                               d.geometry.porosity *
                                               water.density_kg_m3(cell.temperature_k));
            d.cell_water_heat_capacity_j_kg_k.push_back(
                water.heat_capacity_j_kg_k(cell.temperature_k));
            column.push_back({permeability, water.viscosity_pa_s(cell.temperature_k),
                              d.cell_depth_m});
        }

        d.flow = darcy_flow_series(column, region_area_m2, boundaries.delta_p_pa,
                                   coeff.maximum_flow_m3_s);
        d.pore_capacity_kg = 0.0;
        for (double capacity : d.cell_pore_capacity_kg) d.pore_capacity_kg += capacity;

        // A single cell reports its own permeability; a column reports the
        // series-equivalent at the region's rolled-up viscosity, so the
        // chart field stays comparable across cell counts.
        d.permeability_m2 =
            cells.size() == 1
                ? d.cell_permeability_m2.front()
                : (d.flow.resistance_pa_s_m3 > 0.0
                       ? (d.viscosity_pa_s * d.geometry.depth_m) /
                             (d.flow.resistance_pa_s_m3 * region_area_m2)
                       : 0.0);
        derived.push_back(d);
    }
    return {boundaries, derived};
}

bool advance_regions(std::vector<RegionState>& regions, const std::vector<Derived>& derived,
                     const Boundaries& boundaries, const Recipe& recipe,
                     const ModelCoefficients& coeff, const SimulationConfig& config,
                     const WaterProperties& water, double dt, long long step,
                     ShotResult& result, WarningLog& warn, ShotDiagnostics& diag,
                     TerminationReason& termination) {
    bool saturation_invalid = false;
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
        double inflow_mass_kg = water_in_kg;
        double inflow_solids_kg = 0.0;
        double inflow_temperature_k = boundaries.inlet_temperature_k;

        for (std::size_t c = 0; c < region.cells.size(); ++c) {
            CellState& cell = region.cells[c];
            cell.retained_water_kg += inflow_mass_kg;
            cell.dissolved_solids_kg += inflow_solids_kg;

            const double thermal_capacity_j_k =
                cell_dose_kg * coeff.coffee_heat_capacity_j_kg_k +
                std::max(cell.retained_water_kg, kMassEpsilon) *
                    d.cell_water_heat_capacity_j_kg_k[c];
            const double mass_flow_kg_s = inflow_mass_kg / dt;
            const double heat_in_w = mass_flow_kg_s * d.cell_water_heat_capacity_j_kg_k[c] *
                                     (inflow_temperature_k - cell.temperature_k);
            const double heat_loss_w =
                cell_heat_loss_w_k * (cell.temperature_k - coeff.ambient_temperature_k);
            const double dT_dt = (heat_in_w - heat_loss_w) / thermal_capacity_j_k;
            cell.temperature_k += dT_dt * dt;
            if (std::abs(dT_dt * dt) > 5.0) {
                warn.once(result.warnings, "TEMPERATURE_STEP_LARGE",
                          "puck temperature moved more than 5 K in one step; reduce dt_s",
                          state.time_s, WarningSeverity::soft);
            }
            cell.temperature_k = std::clamp(cell.temperature_k, water.min_temperature_k(),
                                            water.max_temperature_k());
            diag.min_puck_temperature_k =
                std::min(diag.min_puck_temperature_k, cell.temperature_k);
            diag.max_puck_temperature_k =
                std::max(diag.max_puck_temperature_k, cell.temperature_k);

            // Extraction reads this cell's own temperature and saturation.
            ShotState cell_view;
            cell_view.puck_temperature_k = cell.temperature_k;
            cell_view.liquid_saturation = cell.liquid_saturation;
            const double k_ext =
                extraction_rate_coefficient(cell_view, recipe, coeff, d.flow.flow_m3_s);
            double extracted_kg = k_ext * cell.remaining_extractable_solids_kg * dt;
            extracted_kg = std::clamp(extracted_kg, 0.0, cell.remaining_extractable_solids_kg);
            cell.remaining_extractable_solids_kg -= extracted_kg;
            cell.dissolved_solids_kg += extracted_kg;
            cell.retained_water_kg += extracted_kg;

            const double capacity_kg = std::max(d.cell_pore_capacity_kg[c], kMassEpsilon);
            double out_kg = std::max(cell.retained_water_kg - capacity_kg, 0.0);
            out_kg = std::min(out_kg, cell.retained_water_kg);
            double solids_out_kg = 0.0;
            if (out_kg > 0.0) {
                const double c_pore =
                    cell.dissolved_solids_kg /
                    std::max(cell.retained_water_kg, kMassEpsilon);
                solids_out_kg = std::min(out_kg * c_pore, cell.dissolved_solids_kg);
                cell.dissolved_solids_kg -= solids_out_kg;
                cell.retained_water_kg -= out_kg;
            }

            cell.liquid_saturation = cell.retained_water_kg / capacity_kg;
            if (cell.liquid_saturation > 1.0 + kSaturationTolerance ||
                cell.liquid_saturation < -kSaturationTolerance) {
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
            cell.liquid_saturation = std::clamp(cell.liquid_saturation, 0.0, 1.0);

            inflow_mass_kg = out_kg;
            inflow_solids_kg = solids_out_kg;
            inflow_temperature_k = cell.temperature_k;
        }
        if (saturation_invalid) break;

        // Whatever the last cell released has left the puck.
        state.beverage_mass_kg += inflow_mass_kg;
        state.dissolved_solids_in_cup_kg += inflow_solids_kg;
        roll_up(region, d, region_dose_kg, coeff);
    }
    if (saturation_invalid) return false;

    for (RegionState& region : regions) {
        region.shot.time_s = static_cast<double>(step + 1) * dt;
    }
    diag.step_count = step + 1;
    return true;
}

void append_sample(ShotResult& result, const ShotState& state, const Boundaries& boundaries,
                   double flow_m3_s, const Recipe& recipe) {
    result.samples.push_back(make_sample(state, boundaries, flow_m3_s, recipe));
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

    if (result.samples.empty() || result.samples.back().time_s < final_state.time_s - 1.0e-9) {
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

    result.manifest.solver_version = std::string(version::kSolver);
    result.manifest.result_schema_version = std::string(version::kResultSchema);
    result.manifest.coefficient_id = coeff.id;
    result.manifest.coefficient_version = coeff.version;
    result.manifest.dt_s = config.dt_s;
    result.manifest.sample_interval_s = config.sample_interval_s;
}

}  // namespace

InvalidInputError::InvalidInputError(const ValidationResult& result)
    : std::runtime_error("invalid simulation input: " + result.summary()), validation_(result) {}

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
    std::vector<RegionState> regions = initialize_regions(recipe, coeff);
    const double initial_extractable_kg = recipe.dose_kg * coeff.extractable_solids_fraction;

    ShotDiagnostics& diag = result.diagnostics;
    diag.min_permeability_m2 = std::numeric_limits<double>::max();
    diag.min_puck_temperature_k = coeff.initial_puck_temperature_k;
    diag.max_puck_temperature_k = coeff.initial_puck_temperature_k;

    const double area_m2 = recipe.basket_area_m2();
    const double dt = config.dt_s;
    TerminationReason termination = TerminationReason::not_terminated;
    double next_sample_time_s = 0.0;

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
        if (aggregate.time_s + 1.0e-9 >= next_sample_time_s) {
            append_sample(result, aggregate, boundaries, flow_m3_s, recipe);
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
        if (!advance_regions(regions, derived, boundaries, recipe, coeff, config, *water_, dt,
                             step, result, warn, diag, termination)) {
            break;
        }

        while (next_sample_time_s <= regions.front().shot.time_s + 1.0e-9) {
            std::vector<RegionState> sampled_regions;
            sampled_regions.reserve(regions.size());
            for (std::size_t i = 0; i < regions.size(); ++i) {
                sampled_regions.push_back(
                    interpolate_region(states_before_step[i], regions[i], next_sample_time_s));
            }
            const auto [sampled_boundaries, sampled_derived] =
                evaluate_regions(sampled_regions, recipe, coeff, *water_, area_m2);
            for (std::size_t i = 0; i < sampled_regions.size(); ++i) {
                sampled_regions[i].shot.permeability_m2 = sampled_derived[i].permeability_m2;
            }
            const ShotState sampled = aggregate_state(sampled_regions, sampled_derived, recipe, coeff);
            append_sample(result, sampled, sampled_boundaries, total_flow(sampled_derived), recipe);
            next_sample_time_s += config.sample_interval_s;
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
