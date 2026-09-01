#include "espressolab/cfd3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#include "cfd3d_geometry.hpp"
#include "cfd3d_pressure.hpp"
#include "espressolab/extraction.hpp"
#include "espressolab/puck.hpp"
#include "espressolab/version.hpp"

namespace espressolab {
namespace {

constexpr double kMassEpsilon = 1.0e-12;
constexpr double kAirViscosityPaS = 1.85e-5;
constexpr std::size_t kInvalidNode = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kMaximumSnapshots = 128;
constexpr std::uint64_t kMaximumSnapshotBytes = 1ULL << 30;
constexpr std::size_t kSnapshotFieldCount = 7;

struct NodeState {
    std::vector<double> retained_water_kg;
    std::vector<double> dissolved_solids_kg;
    std::vector<double> extractable_solids_kg;
    std::vector<double> temperature_k;
    std::vector<double> saturation;
};

struct TransportFace {
    std::size_t a = kInvalidNode;
    std::size_t b = kInvalidNode;
    double total_flux_m3_s = 0.0;
    double water_rate_kg_s = 0.0;
    double water_mass_kg = 0.0;
    double solids_mass_kg = 0.0;
    double energy_j = 0.0;
    std::size_t donor = kInvalidNode;
};

struct OutputFields {
    Cfd3dField pressure;
    Cfd3dField saturation;
    Cfd3dField temperature;
    Cfd3dField pore_tds;
    Cfd3dField permeability_multiplier;
    Cfd3dField velocity_x;
    Cfd3dField velocity_y;
    Cfd3dField velocity_z;
};

struct WaterValues {
    double density_kg_m3 = 0.0;
    double viscosity_pa_s = 0.0;
    double heat_capacity_j_kg_k = 0.0;
};

double node_capacity(const GeometryInternal& geometry, std::size_t active, double porosity,
                     double density_kg_m3) {
    return geometry.aggregate_area_xy_m2[active] * geometry.public_geometry.dz_m * porosity * density_kg_m3;
}

double node_capacity(const GeometryInternal& geometry, std::size_t active, double temperature_k,
                     double porosity, const WaterProperties& water) {
    return node_capacity(geometry, active, porosity, water.density_kg_m3(temperature_k));
}

double concentration(const NodeState& state, std::size_t node) {
    return state.retained_water_kg[node] > kMassEpsilon
               ? state.dissolved_solids_kg[node] / state.retained_water_kg[node]
               : 0.0;
}

std::vector<double> aggregate_material(const GeometryInternal& geometry,
                                        const Cfd3dMaterialField& material) {
    const Cfd3dMesh& mesh = geometry.public_geometry.mesh;
    const std::size_t active_count = geometry.active_roots.size();
    std::vector<double> result(active_count * static_cast<std::size_t>(mesh.nz), 1.0);
    if (material.empty()) return result;
    for (int z = 0; z < mesh.nz; ++z) {
        for (std::size_t xy = 0; xy < geometry.root_for_xy.size(); ++xy) {
            const int root = geometry.root_for_xy[xy];
            if (root < 0) continue;
            const std::size_t active = static_cast<std::size_t>(
                geometry.active_index_for_root[static_cast<std::size_t>(root)]);
            const int x = static_cast<int>(xy % static_cast<std::size_t>(mesh.nx));
            const int y = static_cast<int>(xy / static_cast<std::size_t>(mesh.nx));
            result[node_index(active, z, active_count)] +=
                geometry.public_geometry.cell_area_xy_m2[xy] /
                    geometry.aggregate_area_xy_m2[active] * (material.at(x, y, z) - 1.0);
        }
    }
    return result;
}

std::vector<double> compute_mobility(const GeometryInternal& geometry, const NodeState& state,
                                      const std::vector<double>& material, double porosity,
                                      double absolute_permeability_m2,
                                      const std::vector<WaterValues>& water_values,
                                      const ModelCoefficients& coeff) {
    const std::size_t count = state.retained_water_kg.size();
    const std::size_t active_count = geometry.active_roots.size();
    std::vector<double> mobility(count, 0.0);
    for (std::size_t node = 0; node < count; ++node) {
        const std::size_t active = node % active_count;
        const WaterValues& water = water_values[node];
        const double capacity = node_capacity(geometry, active, porosity, water.density_kg_m3);
        const double saturation = capacity > kMassEpsilon
                                      ? state.retained_water_kg[node] / capacity
                                      : 0.0;
        const double clamped_saturation = std::clamp(saturation, 0.0, 1.0);
        const double permeability = absolute_permeability_m2 * material[node];
        const double water_mobility =
            permeability * wetting_factor(clamped_saturation, coeff.dry_permeability_multiplier) /
            water.viscosity_pa_s;
        const double dry = 1.0 - clamped_saturation;
        const double air_mobility = permeability * std::max(dry * dry, 1.0e-6) /
                                    kAirViscosityPaS;
        mobility[node] = water_mobility + air_mobility;
    }
    return mobility;
}

}  // namespace

Cfd3dField::Cfd3dField(int nx, int ny, int nz, double initial)
    : nx_(nx), ny_(ny), nz_(nz), values_(field_size(nx, ny, nz), initial) {}

Cfd3dMaterialField::Cfd3dMaterialField(int nx, int ny, int nz, double initial)
    : nx_(nx), ny_(ny), nz_(nz), values_(field_size(nx, ny, nz), initial) {}

Cfd3dSolver::Cfd3dSolver() : water_(std::make_shared<TabulatedWaterProperties>()) {}

Cfd3dSolver::Cfd3dSolver(std::shared_ptr<const WaterProperties> water) : water_(std::move(water)) {}

Cfd3dResult Cfd3dSolver::run(const Recipe& recipe, const ModelCoefficients& coeff,
                              const Cfd3dConfig& config,
                              const CancellationCallback& is_cancelled) const {
    ValidationResult validation = recipe.validate();
    validation.merge(coeff.validate());
    const Cfd3dMesh& mesh = config.mesh;
    if (mesh.nx < 1 || mesh.nx > kMaximumMeshNx) {
        validation.add("NONPHYSICAL_INPUT",
                       "cfd3d mesh.nx must be between 1 and " + std::to_string(kMaximumMeshNx),
                       "cfd3d.mesh.nx");
    }
    if (mesh.ny < 1 || mesh.ny > kMaximumMeshNy) {
        validation.add("NONPHYSICAL_INPUT",
                       "cfd3d mesh.ny must be between 1 and " + std::to_string(kMaximumMeshNy),
                       "cfd3d.mesh.ny");
    }
    if (mesh.nz < 1 || mesh.nz > kMaximumMeshNz) {
        validation.add("NONPHYSICAL_INPUT",
                       "cfd3d mesh.nz must be between 1 and " + std::to_string(kMaximumMeshNz),
                       "cfd3d.mesh.nz");
    }
    if (mesh.nx > 0 && mesh.ny > 0 && mesh.nz > 0 &&
        static_cast<std::uint64_t>(mesh.nx) * static_cast<std::uint64_t>(mesh.ny) *
                static_cast<std::uint64_t>(mesh.nz) >
            kMaximumMeshCells) {
        validation.add("NONPHYSICAL_INPUT",
                       "cfd3d mesh cell product must not exceed " + std::to_string(kMaximumMeshCells),
                       "cfd3d.mesh");
    }
    if (!std::isfinite(config.dt_s) || config.dt_s <= 0.0) {
        validation.add("NONPHYSICAL_INPUT", "cfd3d dt_s must be positive", "cfd3d.dt_s");
    }
    if (!std::isfinite(config.sample_interval_s) || config.sample_interval_s <= 0.0) {
        validation.add("NONPHYSICAL_INPUT", "cfd3d sample_interval_s must be positive",
                       "cfd3d.sample_interval_s");
    }
    if (!std::isfinite(config.cfl_number) || config.cfl_number <= 0.0 || config.cfl_number > 1.0) {
        validation.add("OUT_OF_RANGE", "cfd3d cfl_number must be between 0 and 1",
                       "cfd3d.cfl_number");
    }
    if (!std::isfinite(config.pressure_tolerance) || config.pressure_tolerance <= 0.0 ||
        config.pressure_tolerance > 1.0e-8) {
        validation.add("OUT_OF_RANGE", "cfd3d pressure_tolerance must be positive and at most 1e-8",
                       "cfd3d.pressure_tolerance");
    }
    if (config.pressure_max_iterations < 1 || config.pressure_max_iterations > 1000) {
        validation.add("OUT_OF_RANGE", "cfd3d pressure_max_iterations must be between 1 and 1000",
                       "cfd3d.pressure_max_iterations");
    }
    if (!std::isfinite(config.snapshot_interval_s) || config.snapshot_interval_s < 0.0) {
        validation.add("NONPHYSICAL_INPUT", "cfd3d snapshot_interval_s must not be negative",
                       "cfd3d.snapshot_interval_s");
    }
    if (std::isfinite(config.snapshot_interval_s) && config.snapshot_interval_s > 0.0 &&
        mesh.nx > 0 && mesh.ny > 0 && mesh.nz > 0) {
        const double maximum_snapshot_count =
            std::ceil(recipe.maximum_time_s / config.snapshot_interval_s) + 1.0;
        const std::uint64_t field_values =
            static_cast<std::uint64_t>(mesh.nx) * static_cast<std::uint64_t>(mesh.ny) *
            static_cast<std::uint64_t>(mesh.nz);
        const std::uint64_t bytes_per_snapshot =
            field_values * static_cast<std::uint64_t>(kSnapshotFieldCount) * sizeof(double);
        if (maximum_snapshot_count > static_cast<double>(kMaximumSnapshots)) {
            validation.add("OUT_OF_RANGE", "cfd3d snapshot count must not exceed 128",
                           "cfd3d.snapshot_interval_s");
        }
        if (bytes_per_snapshot > 0U &&
            maximum_snapshot_count >
                static_cast<double>(kMaximumSnapshotBytes / bytes_per_snapshot)) {
            validation.add("OUT_OF_RANGE", "cfd3d snapshot output must not exceed 1 GiB",
                           "cfd3d.snapshot_interval_s");
        }
    }
    if (!config.material.empty() &&
        (config.material.x_cells() != mesh.nx || config.material.y_cells() != mesh.ny ||
         config.material.z_cells() != mesh.nz)) {
        validation.add("NONPHYSICAL_INPUT", "cfd3d material field dimensions must match the mesh",
                       "cfd3d.material");
    }
    if (!config.material.empty()) {
        for (std::size_t i = 0; i < config.material.values().size(); ++i) {
            const double value = config.material.values()[i];
            if (!std::isfinite(value) || value < 0.05 || value > 20.0) {
                validation.add("OUT_OF_RANGE",
                               "cfd3d material permeability multipliers must be finite and in [0.05, 20]",
                               "cfd3d.material");
                break;
            }
        }
    }
    if (!water_) {
        validation.add("NONPHYSICAL_INPUT", "cfd3d requires a water-properties provider",
                       "cfd3d.water");
    } else {
        const auto validate_water_properties = [&](double temperature_k) {
            const double density = water_->density_kg_m3(temperature_k);
            const double viscosity = water_->viscosity_pa_s(temperature_k);
            const double heat_capacity = water_->heat_capacity_j_kg_k(temperature_k);
            if (!std::isfinite(density) || density <= 0.0 || !std::isfinite(viscosity) ||
                viscosity <= 0.0 || !std::isfinite(heat_capacity) || heat_capacity <= 0.0) {
                validation.add("NONPHYSICAL_INPUT",
                               "cfd3d water properties must be finite and positive",
                               "cfd3d.water");
            }
        };
        validate_water_properties(coeff.initial_puck_temperature_k);
        for (const ProfilePoint& point : recipe.inlet_temperature_k.points()) {
            validate_water_properties(point.value);
        }
    }
    if (!validation.ok()) throw InvalidInputError(validation);

    Cfd3dResult result;
    result.mesh = mesh;
    result.solver_version = std::string(version::kSolver) + "-cfd3d";
    const double radius_m = recipe.basket_diameter_m * 0.5;
    const PuckGeometry bed =
        compress_puck(recipe, coeff, recipe.pressure_pa.max_value() - coeff.outlet_pressure_pa);
    GeometryInternal geometry = build_geometry(radius_m, bed.depth_m, mesh);
    const bool reduced_uniform_state = material_is_uniform_in_xy(geometry, config.material);
    if (reduced_uniform_state) collapse_uniform_xy_state(geometry);
    result.geometry = geometry.public_geometry;
    result.diagnostics.agglomerated_sliver_count = 0;
    for (const Cfd3dCellClassification classification : result.geometry.classification) {
        if (classification == Cfd3dCellClassification::agglomerated) {
            ++result.diagnostics.agglomerated_sliver_count;
        }
    }

    const double absolute_permeability_m2 =
        kozeny_carman_permeability(recipe.particle_diameter_m, bed.porosity,
                                   coeff.kozeny_constant) *
        distribution_factor(recipe.particle_spread_factor, coeff.distribution_factor_floor);
    const std::vector<double> material = aggregate_material(geometry, config.material);
    const std::size_t active_count = geometry.active_roots.size();
    const std::size_t node_count = active_count * static_cast<std::size_t>(mesh.nz);
    NodeState state;
    state.retained_water_kg.assign(node_count, 0.0);
    state.dissolved_solids_kg.assign(node_count, 0.0);
    state.extractable_solids_kg.assign(node_count, 0.0);
    state.temperature_k.assign(node_count, coeff.initial_puck_temperature_k);
    state.saturation.assign(node_count, 0.0);
    const double basket_area_m2 = recipe.basket_area_m2();
    const double bed_volume_m3 = basket_area_m2 * bed.depth_m;
    for (int z = 0; z < mesh.nz; ++z) {
        for (std::size_t active = 0; active < active_count; ++active) {
            const std::size_t node = node_index(active, z, active_count);
            state.extractable_solids_kg[node] =
                recipe.dose_kg * coeff.extractable_solids_fraction *
                (geometry.aggregate_area_xy_m2[active] * geometry.public_geometry.dz_m) /
                bed_volume_m3;
        }
    }

    double time_s = 0.0;
    double next_sample_s = config.sample_interval_s;
    double next_snapshot_s = config.snapshot_interval_s;
    double cumulative_water_in_kg = 0.0;
    double water_out_kg = 0.0;
    double solids_in_cup_kg = 0.0;
    double beverage_mass_kg = 0.0;
    double inlet_energy_j = 0.0;
    double outlet_energy_j = 0.0;
    double ambient_energy_loss_j = 0.0;
    const double initial_extractable_kg = recipe.dose_kg * coeff.extractable_solids_fraction;
    const double initial_energy_j = recipe.dose_kg * coeff.coffee_heat_capacity_j_kg_k *
                                    coeff.initial_puck_temperature_k;
    std::vector<double> pressure(node_count, 0.0);
    for (int z = 0; z < mesh.nz; ++z) {
        const double fraction = (static_cast<double>(z) + 0.5) / static_cast<double>(mesh.nz);
        for (std::size_t active = 0; active < active_count; ++active) {
            pressure[node_index(active, z, active_count)] =
                recipe.pressure_pa.sample(0.0) * (1.0 - fraction) +
                coeff.outlet_pressure_pa * fraction;
        }
    }

    const auto make_output = [&](const std::vector<double>& current_pressure,
                                 const std::vector<double>& current_mobility,
                                 double current_inlet_pressure_pa) {
        OutputFields fields{Cfd3dField(mesh.nx, mesh.ny, mesh.nz, 0.0),
                            Cfd3dField(mesh.nx, mesh.ny, mesh.nz, 0.0),
                            Cfd3dField(mesh.nx, mesh.ny, mesh.nz, 0.0),
                            Cfd3dField(mesh.nx, mesh.ny, mesh.nz, 0.0),
                            Cfd3dField(mesh.nx, mesh.ny, mesh.nz, 0.0),
                            Cfd3dField(mesh.nx, mesh.ny, mesh.nz, 0.0),
                            Cfd3dField(mesh.nx, mesh.ny, mesh.nz, 0.0),
                            Cfd3dField(mesh.nx, mesh.ny, mesh.nz, 0.0)};
        const auto root_node = [&](int x, int y, int z) -> std::size_t {
            const int root = geometry.root_for_xy[xy_index(x, y, mesh.nx)];
            if (root < 0) return kInvalidNode;
            const int active = geometry.active_index_for_root[static_cast<std::size_t>(root)];
            return node_index(static_cast<std::size_t>(active), z, active_count);
        };
        for (int z = 0; z < mesh.nz; ++z) {
            for (int y = 0; y < mesh.ny; ++y) {
                for (int x = 0; x < mesh.nx; ++x) {
                    const std::size_t node = root_node(x, y, z);
                    if (node == kInvalidNode) continue;
                    fields.pressure.at(x, y, z) = current_pressure[node];
                    fields.saturation.at(x, y, z) = state.saturation[node];
                    fields.temperature.at(x, y, z) = state.temperature_k[node];
                    fields.pore_tds.at(x, y, z) = concentration(state, node);
                    fields.permeability_multiplier.at(x, y, z) =
                        config.material.empty() ? 1.0 : config.material.at(x, y, z);
                }
            }
        }

        const auto face_x_flux = [&](int x, int y, int z) {
            if (x <= 0 || x >= mesh.nx) return 0.0;
            const std::size_t left = root_node(x - 1, y, z);
            const std::size_t right = root_node(x, y, z);
            if (left == kInvalidNode || right == kInvalidNode || left == right) return 0.0;
            const double aperture = geometry.public_geometry.x_face_aperture_m[
                static_cast<std::size_t>(x) + (static_cast<std::size_t>(mesh.nx) + 1U) *
                                                 static_cast<std::size_t>(y)];
            const double transmissibility =
                harmonic_mean(current_mobility[left], current_mobility[right]) * aperture *
                geometry.public_geometry.dz_m / geometry.public_geometry.dx_m;
            return transmissibility * (current_pressure[left] - current_pressure[right]);
        };
        const auto face_y_flux = [&](int x, int y, int z) {
            if (y <= 0 || y >= mesh.ny) return 0.0;
            const std::size_t lower = root_node(x, y - 1, z);
            const std::size_t upper = root_node(x, y, z);
            if (lower == kInvalidNode || upper == kInvalidNode || lower == upper) return 0.0;
            const double aperture = geometry.public_geometry.y_face_aperture_m[
                static_cast<std::size_t>(x) + static_cast<std::size_t>(mesh.nx) *
                                                 static_cast<std::size_t>(y)];
            const double transmissibility =
                harmonic_mean(current_mobility[lower], current_mobility[upper]) * aperture *
                geometry.public_geometry.dz_m / geometry.public_geometry.dy_m;
            return transmissibility * (current_pressure[lower] - current_pressure[upper]);
        };
        const auto face_z_flux = [&](int x, int y, int z) {
            const std::size_t node = root_node(x, y, std::clamp(z, 0, mesh.nz - 1));
            if (node == kInvalidNode) return 0.0;
            const int root = geometry.root_for_xy[xy_index(x, y, mesh.nx)];
            const std::size_t active = static_cast<std::size_t>(
                geometry.active_index_for_root[static_cast<std::size_t>(root)]);
            const double area = geometry.aggregate_area_xy_m2[active];
            if (z == 0) {
                const double coefficient = current_mobility[node] * area /
                                           (0.5 * geometry.public_geometry.dz_m);
                return coefficient * (current_inlet_pressure_pa - current_pressure[node]);
            }
            if (z == mesh.nz) {
                const std::size_t bottom = node_index(active, mesh.nz - 1, active_count);
                const double coefficient = current_mobility[bottom] * area /
                                           (0.5 * geometry.public_geometry.dz_m);
                return coefficient * (current_pressure[bottom] - coeff.outlet_pressure_pa);
            }
            const std::size_t above = node_index(active, z - 1, active_count);
            const std::size_t below = node_index(active, z, active_count);
            const double coefficient =
                harmonic_mean(current_mobility[above], current_mobility[below]) * area /
                geometry.public_geometry.dz_m;
            return coefficient * (current_pressure[above] - current_pressure[below]);
        };
        for (int z = 0; z < mesh.nz; ++z) {
            for (int y = 0; y < mesh.ny; ++y) {
                for (int x = 0; x < mesh.nx; ++x) {
                    const std::size_t node = root_node(x, y, z);
                    if (node == kInvalidNode) continue;
                    const double x_left_aperture =
                        geometry.public_geometry.x_face_aperture_m[
                            static_cast<std::size_t>(x) +
                            (static_cast<std::size_t>(mesh.nx) + 1U) * static_cast<std::size_t>(y)];
                    const double x_right_aperture =
                        geometry.public_geometry.x_face_aperture_m[
                            static_cast<std::size_t>(x + 1) +
                            (static_cast<std::size_t>(mesh.nx) + 1U) * static_cast<std::size_t>(y)];
                    const double y_low_aperture =
                        geometry.public_geometry.y_face_aperture_m[
                            static_cast<std::size_t>(x) + static_cast<std::size_t>(mesh.nx) *
                                                             static_cast<std::size_t>(y)];
                    const double y_high_aperture =
                        geometry.public_geometry.y_face_aperture_m[
                            static_cast<std::size_t>(x) + static_cast<std::size_t>(mesh.nx) *
                                                             static_cast<std::size_t>(y + 1)];
                    const double x_left = face_x_flux(x, y, z);
                    const double x_right = face_x_flux(x + 1, y, z);
                    const double y_low = face_y_flux(x, y, z);
                    const double y_high = face_y_flux(x, y + 1, z);
                    fields.velocity_x.at(x, y, z) =
                        0.5 * ((x_left_aperture > 0.0 ? x_left / (x_left_aperture * geometry.public_geometry.dz_m)
                                                      : 0.0) +
                               (x_right_aperture > 0.0 ?
                                    x_right / (x_right_aperture * geometry.public_geometry.dz_m) :
                                    0.0));
                    fields.velocity_y.at(x, y, z) =
                        0.5 * ((y_low_aperture > 0.0 ? y_low / (y_low_aperture * geometry.public_geometry.dz_m)
                                                     : 0.0) +
                               (y_high_aperture > 0.0 ?
                                    y_high / (y_high_aperture * geometry.public_geometry.dz_m) :
                                    0.0));
                    const double z_top = face_z_flux(x, y, z);
                    const double z_bottom = face_z_flux(x, y, z + 1);
                    const int root = geometry.root_for_xy[xy_index(x, y, mesh.nx)];
                    const std::size_t active = static_cast<std::size_t>(
                        geometry.active_index_for_root[static_cast<std::size_t>(root)]);
                    const double area = geometry.aggregate_area_xy_m2[active];
                    fields.velocity_z.at(x, y, z) = 0.5 * (z_top + z_bottom) / area;
                }
            }
        }
        return fields;
    };

    std::vector<double> last_mobility(node_count, 0.0);
    OutputFields last_output = make_output(pressure, last_mobility, recipe.pressure_pa.sample(0.0));
    double last_snapshot_time = -std::numeric_limits<double>::infinity();
    const auto emit_snapshot = [&](double snapshot_time, const OutputFields& fields) {
        if (!config.snapshot_sink) return;
        if (std::abs(snapshot_time - last_snapshot_time) <= 1.0e-10) return;
        Cfd3dSnapshot snapshot;
        snapshot.time_s = snapshot_time;
        snapshot.pressure_pa = fields.pressure;
        snapshot.saturation = fields.saturation;
        snapshot.temperature_k = fields.temperature;
        snapshot.pore_tds_fraction = fields.pore_tds;
        snapshot.velocity_x_m_s = fields.velocity_x;
        snapshot.velocity_y_m_s = fields.velocity_y;
        snapshot.velocity_z_m_s = fields.velocity_z;
        config.snapshot_sink(snapshot);
        last_snapshot_time = snapshot_time;
    };

    // The node state is immutable for the duration of a timestep. Keep its
    // temperature-dependent water properties together and refresh them only
    // after the next state has been committed.
    std::vector<WaterValues> current_water(node_count);
    const auto refresh_current_water = [&]() {
        for (std::size_t node = 0; node < node_count; ++node) {
            const double temperature_k = state.temperature_k[node];
            current_water[node] = {water_->density_kg_m3(temperature_k),
                                   water_->viscosity_pa_s(temperature_k),
                                   water_->heat_capacity_j_kg_k(temperature_k)};
        }
    };
    refresh_current_water();

    const auto make_sample = [&](double sample_time, double flow_m3_s) {
        double retained_total = 0.0;
        double pore_capacity_total = 0.0;
        double thermal_capacity_total = 0.0;
        double weighted_temperature = 0.0;
        for (std::size_t node = 0; node < node_count; ++node) {
            retained_total += state.retained_water_kg[node];
            const double area = geometry.aggregate_area_xy_m2[node % active_count];
            const double dose_share = recipe.dose_kg * area / basket_area_m2 /
                                      static_cast<double>(mesh.nz);
            const double pore_capacity = node_capacity(geometry, node % active_count, bed.porosity,
                                                       current_water[node].density_kg_m3);
            const double thermal_capacity =
                dose_share * coeff.coffee_heat_capacity_j_kg_k +
                std::max(state.retained_water_kg[node], 0.0) *
                    current_water[node].heat_capacity_j_kg_k;
            pore_capacity_total += pore_capacity;
            thermal_capacity_total += thermal_capacity;
            weighted_temperature += thermal_capacity * state.temperature_k[node];
        }
        ShotSample sample;
        sample.time_s = sample_time;
        sample.pressure_pa = recipe.pressure_pa.sample(sample_time);
        sample.inlet_temperature_k = recipe.inlet_temperature_k.sample(sample_time);
        sample.puck_temperature_k = thermal_capacity_total > kMassEpsilon
                                       ? weighted_temperature / thermal_capacity_total
                                       : coeff.initial_puck_temperature_k;
        sample.flow_m3_s = flow_m3_s;
        sample.beverage_mass_kg = beverage_mass_kg;
        sample.tds_fraction = beverage_mass_kg > kMassEpsilon
                                  ? solids_in_cup_kg / beverage_mass_kg
                                  : 0.0;
        sample.extraction_yield_fraction = recipe.dose_kg > kMassEpsilon
                                                ? solids_in_cup_kg / recipe.dose_kg
                                                : 0.0;
        sample.saturation = pore_capacity_total > kMassEpsilon
                                ? retained_total / pore_capacity_total
                                : 0.0;
        sample.permeability_m2 = absolute_permeability_m2;
        result.samples.push_back(sample);
    };

    PressureSolveResult initial_pressure_solve;
    double last_outflow_volume_m3 = 0.0;
    double last_step_s = config.dt_s;
    bool failed = false;
    while (time_s < recipe.maximum_time_s - 1.0e-12) {
        throw_if_cancelled(is_cancelled);
        const double inlet_pressure_pa = recipe.pressure_pa.sample(time_s);
        const double inlet_temperature_k = recipe.inlet_temperature_k.sample(time_s);
        const double inlet_density = water_->density_kg_m3(inlet_temperature_k);
        const double inlet_heat_capacity = water_->heat_capacity_j_kg_k(inlet_temperature_k);
        last_mobility = compute_mobility(geometry, state, material, bed.porosity,
                                         absolute_permeability_m2, current_water, coeff);
        const PressureLevel pressure_system =
            build_fine_pressure_level(geometry, last_mobility, inlet_pressure_pa,
                                      coeff.outlet_pressure_pa);
        if (time_s == 0.0 && config.snapshot_sink && config.snapshot_initial) {
            initial_pressure_solve = solve_pressure(pressure_system, pressure,
                                                    config.pressure_tolerance,
                                                    config.pressure_max_iterations,
                                                    is_cancelled);
            result.diagnostics.pressure_iterations_total += initial_pressure_solve.iterations;
            result.diagnostics.pressure_residual = initial_pressure_solve.residual;
            if (!initial_pressure_solve.converged) {
                result.termination = TerminationReason::numerical_failure;
                result.warnings.push_back({"NUMERICAL_FAILURE", "cfd3d pressure solve did not converge",
                                           time_s, WarningSeverity::hard});
                failed = true;
                break;
            }
            last_output = make_output(pressure, last_mobility, inlet_pressure_pa);
            emit_snapshot(0.0, last_output);
        } else {
            const PressureSolveResult pressure_solve =
                solve_pressure(pressure_system, pressure, config.pressure_tolerance,
                               config.pressure_max_iterations, is_cancelled);
            result.diagnostics.pressure_iterations_total += pressure_solve.iterations;
            result.diagnostics.pressure_residual = pressure_solve.residual;
            if (!pressure_solve.converged) {
                result.termination = TerminationReason::numerical_failure;
                result.warnings.push_back({"NUMERICAL_FAILURE", "cfd3d pressure solve did not converge",
                                           time_s, WarningSeverity::hard});
                failed = true;
                break;
            }
        }

        if (time_s == 0.0 && result.samples.empty()) make_sample(0.0, 0.0);
        std::vector<TransportFace> faces;
        faces.reserve(pressure_system.edges.size() + active_count * 2U);
        for (const PressureEdge& edge : pressure_system.edges) {
            faces.push_back({edge.a, edge.b, edge.transmissibility * (pressure[edge.a] - pressure[edge.b])});
        }
        for (int z = 0; z < mesh.nz; ++z) {
            for (std::size_t active = 0; active < active_count; ++active) {
                const std::size_t node = node_index(active, z, active_count);
                const double area = geometry.aggregate_area_xy_m2[active];
                const double coefficient =
                    last_mobility[node] * area / (0.5 * geometry.public_geometry.dz_m);
                if (z == 0) {
                    faces.push_back({kInvalidNode, node,
                                     coefficient * (inlet_pressure_pa - pressure[node])});
                }
                if (z + 1 == mesh.nz) {
                    faces.push_back({node, kInvalidNode,
                                     coefficient * (pressure[node] - coeff.outlet_pressure_pa)});
                }
            }
        }

        std::vector<double> total_flux_per_node(node_count, 0.0);
        for (const TransportFace& face : faces) {
            if (face.a != kInvalidNode) total_flux_per_node[face.a] -= face.total_flux_m3_s;
            if (face.b != kInvalidNode) total_flux_per_node[face.b] += face.total_flux_m3_s;
        }
        std::vector<double> water_rate_per_node(node_count, 0.0);
        for (TransportFace& face : faces) {
            if (face.total_flux_m3_s == 0.0) continue;
            if (face.a == kInvalidNode && face.total_flux_m3_s < 0.0) continue;
            if (face.b == kInvalidNode && face.total_flux_m3_s < 0.0) continue;
            face.donor = face.total_flux_m3_s > 0.0 ? face.a : face.b;
            if (face.donor == kInvalidNode) {
                // The only permitted boundary inflow is water from the screen.
                face.water_rate_kg_s = face.total_flux_m3_s * inlet_density;
                face.energy_j = 0.0;
                if (face.b != kInvalidNode) {
                    water_rate_per_node[face.b] += std::abs(face.water_rate_kg_s);
                }
                continue;
            }
            const double fraction = std::clamp(state.saturation[face.donor], 0.0, 1.0);
            const double water_relative =
                wetting_factor(fraction, coeff.dry_permeability_multiplier);
            const double air_relative = std::max((1.0 - fraction) * (1.0 - fraction), 1.0e-6);
            const double permeability = absolute_permeability_m2 * material[face.donor];
            const WaterValues& donor_water = current_water[face.donor];
            const double viscosity = donor_water.viscosity_pa_s;
            const double water_mobility = permeability * water_relative /
                                          viscosity;
            const double air_mobility = permeability * air_relative / kAirViscosityPaS;
            const double total_mobility = water_mobility + air_mobility;
            const double fraction_water = total_mobility > 0.0
                                              ? water_mobility / total_mobility
                                              : 0.0;
            const double rate = face.total_flux_m3_s * fraction_water * donor_water.density_kg_m3;
            face.water_rate_kg_s = rate;
            water_rate_per_node[face.donor] += std::abs(rate);
            if (face.a != kInvalidNode) water_rate_per_node[face.a] += std::abs(rate);
            if (face.b != kInvalidNode) water_rate_per_node[face.b] += std::abs(rate);
        }
        const double max_time_step = std::min(config.dt_s, recipe.maximum_time_s - time_s);
        double dt = max_time_step;
        for (std::size_t node = 0; node < node_count; ++node) {
            const double capacity = node_capacity(geometry, node % active_count, bed.porosity,
                                                  current_water[node].density_kg_m3);
            if (water_rate_per_node[node] > 0.0 && capacity > 0.0) {
                dt = std::min(dt, config.cfl_number * capacity / water_rate_per_node[node]);
            }
        }
        dt = std::min(dt, next_sample_s - time_s);
        if (config.snapshot_interval_s > 0.0) {
            dt = std::min(dt, next_snapshot_s - time_s);
        }
        if (!std::isfinite(dt) || dt <= 0.0) {
            result.termination = TerminationReason::numerical_failure;
            result.warnings.push_back({"NUMERICAL_FAILURE", "cfd3d adaptive timestep became invalid",
                                       time_s, WarningSeverity::hard});
            failed = true;
            break;
        }

        for (TransportFace& face : faces) {
            face.water_mass_kg = face.water_rate_kg_s * dt;
            if (face.donor == kInvalidNode && face.water_mass_kg > 0.0) {
                face.solids_mass_kg = 0.0;
                face.energy_j = face.water_mass_kg * inlet_heat_capacity * inlet_temperature_k;
            } else if (face.donor != kInvalidNode && face.water_mass_kg != 0.0) {
                const double leaving = water_rate_per_node[face.donor] * dt;
                const double scale = leaving > state.retained_water_kg[face.donor]
                                         ? state.retained_water_kg[face.donor] / leaving
                                         : 1.0;
                const double limited_scale = std::clamp(scale, 0.0, 1.0);
                face.water_mass_kg *= limited_scale;
                const double donor_concentration = concentration(state, face.donor);
                const WaterValues& donor_water = current_water[face.donor];
                face.solids_mass_kg = face.water_mass_kg * donor_concentration;
                face.energy_j = face.water_mass_kg * donor_water.heat_capacity_j_kg_k *
                                state.temperature_k[face.donor];
            }
        }

        std::vector<double> net_water(node_count, 0.0);
        std::vector<double> net_solids(node_count, 0.0);
        std::vector<double> net_energy(node_count, 0.0);
        std::vector<double> downward_flux(node_count, 0.0);
        double inflow_kg = 0.0;
        double outflow_kg = 0.0;
        double outflow_solids = 0.0;
        double outflow_volume = 0.0;
        for (const TransportFace& face : faces) {
            if (face.a != kInvalidNode) {
                net_water[face.a] -= face.water_mass_kg;
                net_solids[face.a] -= face.solids_mass_kg;
                net_energy[face.a] -= face.energy_j;
            }
            if (face.b != kInvalidNode) {
                net_water[face.b] += face.water_mass_kg;
                net_solids[face.b] += face.solids_mass_kg;
                net_energy[face.b] += face.energy_j;
            }
            if (face.a == kInvalidNode && face.b != kInvalidNode && face.water_mass_kg > 0.0) {
                inflow_kg += face.water_mass_kg;
                inlet_energy_j += face.energy_j;
            }
            if (face.b == kInvalidNode && face.a != kInvalidNode && face.water_mass_kg > 0.0) {
                outflow_kg += face.water_mass_kg;
                outflow_solids += face.solids_mass_kg;
                outflow_volume += face.water_mass_kg /
                                  current_water[face.a].density_kg_m3;
                outlet_energy_j += face.energy_j;
            }
        }
        for (const PressureEdge& edge : pressure_system.edges) {
            if (edge.axis != 2) continue;
            const double flux = edge.transmissibility * (pressure[edge.a] - pressure[edge.b]);
            if (flux > downward_flux[edge.a]) downward_flux[edge.a] = flux;
        }
        for (int z = 0; z < mesh.nz; ++z) {
            for (std::size_t active = 0; active < active_count; ++active) {
                if (z + 1 != mesh.nz) continue;
                const std::size_t node = node_index(active, z, active_count);
                const double area = geometry.aggregate_area_xy_m2[active];
                const double coefficient =
                    last_mobility[node] * area / (0.5 * geometry.public_geometry.dz_m);
                downward_flux[node] = std::max(
                    downward_flux[node], coefficient * (pressure[node] - coeff.outlet_pressure_pa));
            }
        }

        NodeState next = state;
        bool invalid_state = false;
        for (std::size_t node = 0; node < node_count; ++node) {
            const double old_temperature = state.temperature_k[node];
            const double retained = state.retained_water_kg[node] + net_water[node];
            const double cp = current_water[node].heat_capacity_j_kg_k;
            const double dose_share =
                recipe.dose_kg * geometry.aggregate_area_xy_m2[node % active_count] / basket_area_m2 /
                static_cast<double>(mesh.nz);
            const double loss_power = coeff.ambient_heat_loss_w_k *
                                      (geometry.aggregate_area_xy_m2[node % active_count] / basket_area_m2) /
                                      static_cast<double>(mesh.nz) *
                                      (old_temperature - coeff.ambient_temperature_k);
            const double old_energy = dose_share * coeff.coffee_heat_capacity_j_kg_k * old_temperature +
                                      state.retained_water_kg[node] * cp * old_temperature;
            const double energy_after_flux = old_energy + net_energy[node] - loss_power * dt;
            const double retained_for_energy = std::max(retained, 0.0);
            double updated_temperature = old_temperature;
            // Use a fixed correction count so the temperature-dependent water
            // capacity preserves the explicit internal-energy balance.
            for (int correction = 0; correction < 8; ++correction) {
                const double updated_cp = water_->heat_capacity_j_kg_k(updated_temperature);
                updated_temperature = energy_after_flux /
                                      std::max(dose_share * coeff.coffee_heat_capacity_j_kg_k +
                                                   retained_for_energy * updated_cp,
                                               kMassEpsilon);
            }
            const double extracted = std::clamp(
                extraction_rate_coefficient(
                    ShotState{time_s, updated_temperature, 0.0, state.saturation[node],
                              state.extractable_solids_kg[node], state.dissolved_solids_kg[node], 0.0,
                              0.0, state.retained_water_kg[node], 0.0, {}},
                    recipe, coeff,
                    std::max(downward_flux[node], 0.0) /
                        std::max(geometry.aggregate_area_xy_m2[node % active_count], kMassEpsilon) *
                        basket_area_m2) *
                    state.extractable_solids_kg[node] * dt,
                0.0, state.extractable_solids_kg[node]);
            const double dissolved = state.dissolved_solids_kg[node] + net_solids[node] + extracted;
            const double capacity = node_capacity(geometry, node % active_count, updated_temperature,
                                                  bed.porosity, *water_);
            const double saturation = capacity > kMassEpsilon ? retained / capacity : 0.0;
            const double tolerance = 1.0e-9 * std::max(capacity, 1.0);
            if (!std::isfinite(retained) || !std::isfinite(dissolved) ||
                !std::isfinite(updated_temperature) || !std::isfinite(saturation) ||
                retained < -tolerance || dissolved < -tolerance || saturation < -1.0e-9 ||
                saturation > 1.0 + 1.0e-9 || updated_temperature < water_->min_temperature_k() - 1.0e-9 ||
                updated_temperature > water_->max_temperature_k() + 1.0e-9) {
                invalid_state = true;
                break;
            }
            next.retained_water_kg[node] = std::max(0.0, retained);
            next.dissolved_solids_kg[node] = std::max(0.0, dissolved);
            next.extractable_solids_kg[node] = state.extractable_solids_kg[node] - extracted;
            next.temperature_k[node] = std::clamp(updated_temperature, water_->min_temperature_k(),
                                                   water_->max_temperature_k());
            next.saturation[node] = std::clamp(saturation, 0.0, 1.0);
            result.diagnostics.max_courant_number =
                std::max(result.diagnostics.max_courant_number,
                         capacity > kMassEpsilon ? std::abs(net_water[node]) / capacity : 0.0);
        }
        if (invalid_state) {
            ++result.diagnostics.nonfinite_state_count;
            result.termination = TerminationReason::invalid_state;
            result.warnings.push_back({"INVALID_STATE", "cfd3d state failed finite, positive, or saturation checks",
                                       time_s, WarningSeverity::hard});
            failed = true;
            break;
        }

        for (std::size_t node = 0; node < node_count; ++node) {
            const std::size_t active = node % active_count;
            const double area = geometry.aggregate_area_xy_m2[active];
            result.diagnostics.max_total_velocity_divergence_1_s =
                std::max(result.diagnostics.max_total_velocity_divergence_1_s,
                         std::abs(total_flux_per_node[node]) /
                             std::max(area * geometry.public_geometry.dz_m, kMassEpsilon));
            const double loss_power = coeff.ambient_heat_loss_w_k * (area / basket_area_m2) /
                                      static_cast<double>(mesh.nz) *
                                      (state.temperature_k[node] - coeff.ambient_temperature_k);
            ambient_energy_loss_j += loss_power * dt;
        }
        state = std::move(next);
        refresh_current_water();
        cumulative_water_in_kg += inflow_kg;
        water_out_kg += outflow_kg;
        solids_in_cup_kg += outflow_solids;
        beverage_mass_kg += outflow_kg + outflow_solids;
        last_outflow_volume_m3 = outflow_volume;
        last_step_s = dt;
        time_s += dt;
        ++result.diagnostics.step_count;

        if (time_s + 1.0e-10 >= next_sample_s) {
            make_sample(next_sample_s, last_step_s > 0.0 ? last_outflow_volume_m3 / last_step_s : 0.0);
            next_sample_s += config.sample_interval_s;
        }
        if (config.snapshot_interval_s > 0.0 && time_s + 1.0e-10 >= next_snapshot_s) {
            last_output = make_output(pressure, last_mobility, inlet_pressure_pa);
            if (config.snapshot_sink) emit_snapshot(next_snapshot_s, last_output);
            next_snapshot_s += config.snapshot_interval_s;
        }
        if (recipe.target_beverage_mass_kg.has_value() &&
            beverage_mass_kg >= *recipe.target_beverage_mass_kg) {
            result.termination = TerminationReason::target_mass_reached;
            break;
        }
        if (time_s >= recipe.maximum_time_s - 1.0e-12) {
            result.termination = TerminationReason::time_limit_reached;
            break;
        }
    }

    if (!failed && result.termination == TerminationReason::not_terminated) {
        result.termination = TerminationReason::time_limit_reached;
    }

    if (!failed) {
        const double inlet_pressure_pa = recipe.pressure_pa.sample(time_s);
        last_mobility = compute_mobility(geometry, state, material, bed.porosity,
                                         absolute_permeability_m2, current_water, coeff);
        const PressureLevel final_system =
            build_fine_pressure_level(geometry, last_mobility, inlet_pressure_pa,
                                      coeff.outlet_pressure_pa);
        const PressureSolveResult final_pressure =
            solve_pressure(final_system, pressure, config.pressure_tolerance,
                           config.pressure_max_iterations, is_cancelled);
        result.diagnostics.pressure_iterations_total += final_pressure.iterations;
        result.diagnostics.pressure_residual = final_pressure.residual;
        if (!final_pressure.converged) {
            result.termination = TerminationReason::numerical_failure;
            result.warnings.push_back({"NUMERICAL_FAILURE", "cfd3d final pressure solve did not converge",
                                       time_s, WarningSeverity::hard});
            failed = true;
        } else {
            last_output = make_output(pressure, last_mobility, inlet_pressure_pa);
        }
    }

    if (config.snapshot_sink && config.snapshot_final && !failed) {
        emit_snapshot(time_s, last_output);
    }
    if (!failed && (result.samples.empty() ||
                    std::abs(result.samples.back().time_s - time_s) > 1.0e-10)) {
        make_sample(time_s, last_step_s > 0.0 ? last_outflow_volume_m3 / last_step_s : 0.0);
    }

    double retained_total = 0.0;
    double dissolved_total = 0.0;
    double extractable_total = 0.0;
    double stored_energy_j = 0.0;
    for (std::size_t node = 0; node < node_count; ++node) {
        retained_total += state.retained_water_kg[node];
        dissolved_total += state.dissolved_solids_kg[node];
        extractable_total += state.extractable_solids_kg[node];
        stored_energy_j +=
            recipe.dose_kg * geometry.aggregate_area_xy_m2[node % active_count] / basket_area_m2 /
                static_cast<double>(mesh.nz) * coeff.coffee_heat_capacity_j_kg_k *
                state.temperature_k[node];
        stored_energy_j += state.retained_water_kg[node] *
                           current_water[node].heat_capacity_j_kg_k *
                           state.temperature_k[node];
    }
    result.diagnostics.water_mass_residual_kg =
        cumulative_water_in_kg - retained_total - water_out_kg;
    result.diagnostics.solids_mass_residual_kg =
        initial_extractable_kg - extractable_total - dissolved_total - solids_in_cup_kg;
    result.diagnostics.energy_residual_j =
        initial_energy_j + inlet_energy_j - outlet_energy_j - ambient_energy_loss_j - stored_energy_j;
    result.elapsed_time_s = time_s;
    result.beverage_mass_kg = beverage_mass_kg;
    result.tds_fraction = beverage_mass_kg > kMassEpsilon
                              ? solids_in_cup_kg / beverage_mass_kg
                              : 0.0;
    result.extraction_yield_fraction = recipe.dose_kg > kMassEpsilon
                                           ? solids_in_cup_kg / recipe.dose_kg
                                           : 0.0;
    if (result.termination == TerminationReason::not_terminated) {
        result.termination = TerminationReason::time_limit_reached;
    }

    result.pressure_pa = last_output.pressure;
    result.saturation = last_output.saturation;
    result.temperature_k = last_output.temperature;
    result.pore_tds_fraction = last_output.pore_tds;
    result.permeability_multiplier = last_output.permeability_multiplier;
    result.velocity_x_m_s = last_output.velocity_x;
    result.velocity_y_m_s = last_output.velocity_y;
    result.velocity_z_m_s = last_output.velocity_z;
    return result;
}

}  // namespace espressolab
