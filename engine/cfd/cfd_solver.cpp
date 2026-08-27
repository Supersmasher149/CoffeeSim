#include "espressolab/cfd.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "espressolab/extraction.hpp"
#include "espressolab/simulator.hpp"
#include "espressolab/puck.hpp"
#include "espressolab/units.hpp"
#include "espressolab/version.hpp"

namespace espressolab {
namespace {

constexpr double kMassEpsilon = 1.0e-12;
constexpr double kPi = std::numbers::pi;

// Air is the non-wetting phase. It escapes freely through the basket, so it is
// carried only through its mobility: it is what lets a dry cell fill instead of
// the incompressible solve refusing to accumulate anything.
constexpr double kAirViscosityPaS = 1.85e-5;

// Axisymmetric mesh metrics. r runs from the axis to the basket wall, z from
// the dispersion screen down to the basket floor.
struct Geometry {
    int nr = 0;
    int nz = 0;
    double dr = 0.0;
    double dz = 0.0;
    std::vector<double> face_radius_m;    // nr + 1 entries
    std::vector<double> cell_volume_m3;   // per radial ring, times dz
    std::vector<double> axial_area_m2;    // per radial ring
    std::vector<double> radial_area_m2;   // at each interior radial face, times dz

    [[nodiscard]] double volume(int i) const { return cell_volume_m3[static_cast<std::size_t>(i)]; }
};

Geometry build_geometry(double radius_m, double depth_m, const CfdMesh& mesh) {
    Geometry g;
    g.nr = mesh.radial_cells;
    g.nz = mesh.axial_cells;
    g.dr = radius_m / static_cast<double>(g.nr);
    g.dz = depth_m / static_cast<double>(g.nz);
    g.face_radius_m.resize(static_cast<std::size_t>(g.nr) + 1);
    for (int i = 0; i <= g.nr; ++i) {
        g.face_radius_m[static_cast<std::size_t>(i)] = static_cast<double>(i) * g.dr;
    }
    g.axial_area_m2.resize(static_cast<std::size_t>(g.nr));
    g.cell_volume_m3.resize(static_cast<std::size_t>(g.nr));
    g.radial_area_m2.resize(static_cast<std::size_t>(g.nr) + 1);
    for (int i = 0; i < g.nr; ++i) {
        const double r_in = g.face_radius_m[static_cast<std::size_t>(i)];
        const double r_out = g.face_radius_m[static_cast<std::size_t>(i) + 1];
        const double ring = kPi * (r_out * r_out - r_in * r_in);
        g.axial_area_m2[static_cast<std::size_t>(i)] = ring;
        g.cell_volume_m3[static_cast<std::size_t>(i)] = ring * g.dz;
    }
    for (int i = 0; i <= g.nr; ++i) {
        // The axis face has zero area, which is the symmetry condition.
        g.radial_area_m2[static_cast<std::size_t>(i)] =
            2.0 * kPi * g.face_radius_m[static_cast<std::size_t>(i)] * g.dz;
    }
    return g;
}

// Relative permeability. Water mobilises as the pore fills; air does the
// reverse. Both are bounded away from a hard zero so the pressure matrix stays
// non-singular in a fully dry or fully wet cell.
double water_relative_permeability(double saturation, double dry_multiplier) {
    return wetting_factor(std::clamp(saturation, 0.0, 1.0), dry_multiplier);
}

double air_relative_permeability(double saturation) {
    const double s = std::clamp(saturation, 0.0, 1.0);
    const double dry = 1.0 - s;
    return std::max(dry * dry, 1.0e-6);
}

double harmonic_mean(double a, double b) {
    if (a <= 0.0 || b <= 0.0) return 0.0;
    return 2.0 * a * b / (a + b);
}

struct WaterValues {
    double density_kg_m3;
    double viscosity_pa_s;
    double heat_capacity_j_kg_k;
};

struct WaterTemperatureRange {
    double min_temperature_k;
    double max_temperature_k;
};

WaterValues water_values_at(const WaterProperties& water, double temperature_k) {
    ValidationResult validation;
    if (!std::isfinite(temperature_k)) {
        validation.add("NONFINITE_INPUT", "water-property temperature must be finite",
                       "cfd.water.temperature_k");
    }

    const double density = water.density_kg_m3(temperature_k);
    const double viscosity = water.viscosity_pa_s(temperature_k);
    const double heat_capacity = water.heat_capacity_j_kg_k(temperature_k);
    if (!std::isfinite(density) || density <= 0.0) {
        validation.add("NONPHYSICAL_INPUT", "cfd water density must be finite and positive",
                       "cfd.water.density_kg_m3");
    }
    if (!std::isfinite(viscosity) || viscosity <= 0.0) {
        validation.add("NONPHYSICAL_INPUT", "cfd water viscosity must be finite and positive",
                       "cfd.water.viscosity_pa_s");
    }
    if (!std::isfinite(heat_capacity) || heat_capacity <= 0.0) {
        validation.add("NONPHYSICAL_INPUT",
                       "cfd water heat capacity must be finite and positive",
                       "cfd.water.heat_capacity_j_kg_k");
    }
    if (!validation.ok()) throw InvalidInputError(validation);
    return {density, viscosity, heat_capacity};
}

WaterTemperatureRange validate_water_provider(const WaterProperties& water,
                                              const Recipe& recipe,
                                              const ModelCoefficients& coeff) {
    const double min_temperature_k = water.min_temperature_k();
    const double max_temperature_k = water.max_temperature_k();
    ValidationResult validation;
    if (!std::isfinite(min_temperature_k) || !std::isfinite(max_temperature_k) ||
        min_temperature_k <= 0.0 || max_temperature_k <= 0.0 ||
        min_temperature_k > max_temperature_k) {
        validation.add("NONPHYSICAL_INPUT",
                       "cfd water temperature bounds must be finite, positive, and ordered",
                       "cfd.water.temperature_range");
    }
    if (!validation.ok()) throw InvalidInputError(validation);

    // Probe the provider at its advertised bounds and every temperature the
    // recipe supplies before entering the numerical loop.
    water_values_at(water, min_temperature_k);
    water_values_at(water, max_temperature_k);
    water_values_at(water, coeff.initial_puck_temperature_k);
    for (const ProfilePoint& point : recipe.inlet_temperature_k.points()) {
        water_values_at(water, point.value);
    }
    return {min_temperature_k, max_temperature_k};
}

bool all_finite(const CfdField& field) {
    return std::all_of(field.values().begin(), field.values().end(),
                       [](double value) { return std::isfinite(value); });
}

bool all_finite(const ShotSample& sample) {
    return std::isfinite(sample.time_s) && std::isfinite(sample.pressure_pa) &&
           std::isfinite(sample.inlet_temperature_k) && std::isfinite(sample.puck_temperature_k) &&
           std::isfinite(sample.flow_m3_s) && std::isfinite(sample.beverage_mass_kg) &&
           std::isfinite(sample.tds_fraction) &&
           std::isfinite(sample.extraction_yield_fraction) && std::isfinite(sample.saturation) &&
           std::isfinite(sample.permeability_m2);
}

bool all_finite(const CfdResult& result) {
    const CfdDiagnostics& diagnostics = result.diagnostics;
    const bool diagnostics_finite =
        std::isfinite(diagnostics.max_total_velocity_divergence_1_s) &&
        std::isfinite(diagnostics.pressure_residual) &&
        std::isfinite(diagnostics.water_mass_residual_kg) &&
        std::isfinite(diagnostics.solids_mass_residual_kg) &&
        std::isfinite(diagnostics.max_courant_number);
    return diagnostics_finite && std::isfinite(result.elapsed_time_s) &&
           std::isfinite(result.beverage_mass_kg) && std::isfinite(result.tds_fraction) &&
           std::isfinite(result.extraction_yield_fraction) && all_finite(result.pressure_pa) &&
           all_finite(result.saturation) && all_finite(result.temperature_k) &&
           all_finite(result.pore_tds_fraction) && all_finite(result.axial_velocity_m_s) &&
           all_finite(result.radial_velocity_m_s) &&
           std::all_of(result.samples.begin(), result.samples.end(),
                       [](const ShotSample& sample) { return all_finite(sample); });
}

}  // namespace

CfdSolver::CfdSolver() : water_(std::make_shared<TabulatedWaterProperties>()) {}
CfdSolver::CfdSolver(std::shared_ptr<const WaterProperties> water) : water_(std::move(water)) {
    if (!water_) {
        ValidationResult validation;
        validation.add("NONPHYSICAL_INPUT", "cfd requires a water-properties provider",
                       "cfd.water");
        throw InvalidInputError(validation);
    }
}

CfdResult CfdSolver::run(const Recipe& recipe, const ModelCoefficients& coeff,
                         const CfdConfig& config,
                         const CancellationCallback& is_cancelled) const {
    ValidationResult validation = recipe.validate();
    validation.merge(coeff.validate());
    if (config.mesh.radial_cells < 1 || config.mesh.radial_cells > 128) {
        validation.add("NONPHYSICAL_INPUT", "cfd mesh.radial_cells must be between 1 and 128",
                       "cfd.mesh.radial_cells");
    }
    if (config.mesh.axial_cells < 1 || config.mesh.axial_cells > 256) {
        validation.add("NONPHYSICAL_INPUT", "cfd mesh.axial_cells must be between 1 and 256",
                       "cfd.mesh.axial_cells");
    }
    if (!std::isfinite(config.dt_s) || !std::isfinite(config.sample_interval_s) ||
        config.dt_s <= 0.0 || config.sample_interval_s <= 0.0) {
        validation.add("NONPHYSICAL_INPUT", "cfd dt_s and sample_interval_s must be positive",
                       "cfd.dt_s");
    }
    if (!std::isfinite(config.pressure_tolerance) || config.pressure_tolerance <= 0.0) {
        validation.add("NONPHYSICAL_INPUT", "cfd pressure_tolerance must be positive",
                       "cfd.pressure_tolerance");
    }
    if (config.pressure_max_iterations < 1) {
        validation.add("NONPHYSICAL_INPUT", "cfd pressure_max_iterations must be positive",
                       "cfd.pressure_max_iterations");
    }
    if (!std::isfinite(config.relaxation) || config.relaxation <= 0.0 || config.relaxation > 2.0) {
        validation.add("OUT_OF_RANGE", "cfd relaxation must be between 0 and 2",
                       "cfd.relaxation");
    }
    if (!validation.ok()) throw InvalidInputError(validation);
    const WaterTemperatureRange water_range = validate_water_provider(*water_, recipe, coeff);

    CfdResult result;
    result.mesh = config.mesh;
    result.solver_version = std::string(version::kSolver);

    const int nr = config.mesh.radial_cells;
    const int nz = config.mesh.axial_cells;
    const double radius_m = recipe.basket_diameter_m * 0.5;

    // The bed compresses under the commanded pressure exactly as at Level 1-3;
    // the mesh is built on the compressed depth at the peak commanded pressure
    // so the geometry stays fixed while the fields evolve.
    const PuckGeometry bed =
        compress_puck(recipe, coeff, recipe.pressure_pa.max_value() - coeff.outlet_pressure_pa);
    const Geometry g = build_geometry(radius_m, bed.depth_m, config.mesh);
    const double porosity = bed.porosity;

    const double absolute_permeability_m2 =
        kozeny_carman_permeability(recipe.particle_diameter_m, porosity, coeff.kozeny_constant) *
        distribution_factor(recipe.particle_spread_factor, coeff.distribution_factor_floor);

    // Per-cell permeability multiplier, mapped from the lateral regions onto
    // the radial mesh by area so a channelled recipe keeps its meaning here.
    CfdField multiplier(nr, nz, 1.0);
    {
        std::vector<double> region_edges;
        double cumulative = 0.0;
        for (const ParallelRegion& region : recipe.parallel_regions) {
            cumulative += region.area_fraction;
            region_edges.push_back(cumulative);
        }
        for (int i = 0; i < nr; ++i) {
            const double r_in = g.face_radius_m[static_cast<std::size_t>(i)];
            const double r_out = g.face_radius_m[static_cast<std::size_t>(i) + 1];
            const double area_fraction_mid =
                (r_in * r_in + r_out * r_out) * 0.5 / (radius_m * radius_m);
            std::size_t region = 0;
            while (region + 1 < region_edges.size() && area_fraction_mid > region_edges[region]) {
                ++region;
            }
            for (int j = 0; j < nz; ++j) {
                multiplier.at(i, j) = recipe.parallel_regions[region].permeability_multiplier;
            }
        }
    }

    CfdField pressure(nr, nz, coeff.outlet_pressure_pa);
    CfdField saturation(nr, nz, 0.0);
    CfdField temperature(nr, nz, coeff.initial_puck_temperature_k);
    // Water and solute are tracked separately: the pore volume is filled by
    // water, and dissolved solids ride in it as a dilute solute. Folding solute
    // into the pore liquid mass, as the lumped model does, would let extraction
    // push saturation past one with no mechanism to expel the excess.
    CfdField dissolved_kg(nr, nz, 0.0);
    CfdField retained_kg(nr, nz, 0.0);  // water only
    CfdField extractable_kg(nr, nz, 0.0);

    const double bed_volume_m3 = kPi * radius_m * radius_m * bed.depth_m;
    for (int j = 0; j < nz; ++j) {
        for (int i = 0; i < nr; ++i) {
            const double share = g.volume(i) / bed_volume_m3;
            extractable_kg.at(i, j) = recipe.dose_kg * coeff.extractable_solids_fraction * share;
        }
    }

    CfdDiagnostics& diag = result.diagnostics;
    double cumulative_water_in_kg = 0.0;
    double water_out_kg = 0.0;
    double beverage_mass_kg = 0.0;
    double solids_in_cup_kg = 0.0;
    const double initial_extractable_kg = recipe.dose_kg * coeff.extractable_solids_fraction;

    std::vector<double> last_flux_z(static_cast<std::size_t>(nr) * static_cast<std::size_t>(nz + 1), 0.0);
    std::vector<double> last_flux_r(static_cast<std::size_t>(nr + 1) * static_cast<std::size_t>(nz), 0.0);
    std::vector<double> total_mobility(static_cast<std::size_t>(nr) * static_cast<std::size_t>(nz));
    std::vector<double> water_fraction(static_cast<std::size_t>(nr) * static_cast<std::size_t>(nz));

    double time_s = 0.0;
    double next_sample_s = 0.0;
    TerminationReason termination = TerminationReason::not_terminated;
    const auto cell = [nr](int i, int j) {
        return static_cast<std::size_t>(j) * static_cast<std::size_t>(nr) +
               static_cast<std::size_t>(i);
    };

    for (long long step = 0;; ++step) {
        throw_if_cancelled(is_cancelled);
        const double inlet_pressure_pa = recipe.pressure_pa.sample(time_s);
        const double inlet_temperature_k = recipe.inlet_temperature_k.sample(time_s);

        // ---- mobilities -------------------------------------------------
        for (int j = 0; j < nz; ++j) {
            for (int i = 0; i < nr; ++i) {
                const double k_abs = absolute_permeability_m2 * multiplier.at(i, j);
                const double mu_w = water_values_at(*water_, temperature.at(i, j)).viscosity_pa_s;
                const double lambda_w =
                    k_abs * water_relative_permeability(saturation.at(i, j),
                                                        coeff.dry_permeability_multiplier) /
                    std::max(mu_w, kMassEpsilon);
                const double lambda_a =
                    k_abs * air_relative_permeability(saturation.at(i, j)) / kAirViscosityPaS;
                const double lambda_t = lambda_w + lambda_a;
                total_mobility[cell(i, j)] = lambda_t;
                water_fraction[cell(i, j)] = lambda_t > 0.0 ? lambda_w / lambda_t : 0.0;
            }
        }

        // ---- elliptic pressure solve: div(lambda_t grad p) = 0 -----------
        // Dirichlet at the screen and the basket floor, no flow at the wall,
        // symmetry on the axis. Red-black SOR keeps the sweep order fixed and
        // therefore the result reproducible.
        double residual = 0.0;
        long long iterations = 0;
        bool pressure_converged = false;
        for (; iterations < config.pressure_max_iterations; ++iterations) {
            if ((iterations & 31) == 0) throw_if_cancelled(is_cancelled);
            double max_residual = 0.0;
            double max_scale = 0.0;
            for (int colour = 0; colour < 2; ++colour) {
                for (int j = 0; j < nz; ++j) {
                    for (int i = 0; i < nr; ++i) {
                        if (((i + j) & 1) != colour) continue;
                        const double lambda_c = total_mobility[cell(i, j)];
                        double diagonal = 0.0;
                        double off = 0.0;

                        // radial faces
                        if (i > 0) {
                            const double t = harmonic_mean(lambda_c, total_mobility[cell(i - 1, j)]) *
                                             g.radial_area_m2[static_cast<std::size_t>(i)] / g.dr;
                            diagonal += t;
                            off += t * pressure.at(i - 1, j);
                        }
                        if (i < nr - 1) {
                            const double t = harmonic_mean(lambda_c, total_mobility[cell(i + 1, j)]) *
                                             g.radial_area_m2[static_cast<std::size_t>(i) + 1] / g.dr;
                            diagonal += t;
                            off += t * pressure.at(i + 1, j);
                        }
                        // axial faces, with half-cell distance to each boundary
                        const double area_z = g.axial_area_m2[static_cast<std::size_t>(i)];
                        if (j > 0) {
                            const double t = harmonic_mean(lambda_c, total_mobility[cell(i, j - 1)]) *
                                             area_z / g.dz;
                            diagonal += t;
                            off += t * pressure.at(i, j - 1);
                        } else {
                            const double t = lambda_c * area_z / (0.5 * g.dz);
                            diagonal += t;
                            off += t * inlet_pressure_pa;
                        }
                        if (j < nz - 1) {
                            const double t = harmonic_mean(lambda_c, total_mobility[cell(i, j + 1)]) *
                                             area_z / g.dz;
                            diagonal += t;
                            off += t * pressure.at(i, j + 1);
                        } else {
                            const double t = lambda_c * area_z / (0.5 * g.dz);
                            diagonal += t;
                            off += t * coeff.outlet_pressure_pa;
                        }

                        if (diagonal <= 0.0) continue;
                        const double updated = off / diagonal;
                        const double change = updated - pressure.at(i, j);
                        pressure.at(i, j) += config.relaxation * change;
                        max_residual = std::max(max_residual, std::abs(change * diagonal));
                        max_scale = std::max(max_scale, std::abs(off));
                    }
                }
            }
            residual = max_scale > 0.0 ? max_residual / max_scale : max_residual;
            if (residual < config.pressure_tolerance) {
                pressure_converged = true;
                break;
            }
        }
        diag.pressure_iterations_total += iterations;
        diag.pressure_residual = residual;
        if (!pressure_converged) {
            termination = TerminationReason::numerical_failure;
            result.warnings.push_back({"NUMERICAL_FAILURE", "pressure solve did not converge",
                                       time_s, WarningSeverity::hard});
            break;
        }

        // ---- face fluxes from Darcy, and the divergence check ------------
        // Axial face fluxes: index j is the face above cell j.
        std::vector<double> flux_z(static_cast<std::size_t>(nr) * static_cast<std::size_t>(nz + 1), 0.0);
        std::vector<double> flux_r(static_cast<std::size_t>(nr + 1) * static_cast<std::size_t>(nz), 0.0);
        const auto zface = [nr](int i, int j) {
            return static_cast<std::size_t>(j) * static_cast<std::size_t>(nr) +
                   static_cast<std::size_t>(i);
        };
        const auto rface = [nr](int i, int j) {
            return static_cast<std::size_t>(j) * (static_cast<std::size_t>(nr) + 1) +
                   static_cast<std::size_t>(i);
        };

        for (int j = 0; j <= nz; ++j) {
            for (int i = 0; i < nr; ++i) {
                const double area_z = g.axial_area_m2[static_cast<std::size_t>(i)];
                if (j == 0) {
                    const double t = total_mobility[cell(i, 0)] * area_z / (0.5 * g.dz);
                    flux_z[zface(i, j)] = t * (inlet_pressure_pa - pressure.at(i, 0));
                } else if (j == nz) {
                    const double t = total_mobility[cell(i, nz - 1)] * area_z / (0.5 * g.dz);
                    flux_z[zface(i, j)] = t * (pressure.at(i, nz - 1) - coeff.outlet_pressure_pa);
                } else {
                    const double t =
                        harmonic_mean(total_mobility[cell(i, j - 1)], total_mobility[cell(i, j)]) *
                        area_z / g.dz;
                    flux_z[zface(i, j)] = t * (pressure.at(i, j - 1) - pressure.at(i, j));
                }
            }
        }
        for (int j = 0; j < nz; ++j) {
            for (int i = 1; i < nr; ++i) {
                const double t =
                    harmonic_mean(total_mobility[cell(i - 1, j)], total_mobility[cell(i, j)]) *
                    g.radial_area_m2[static_cast<std::size_t>(i)] / g.dr;
                flux_r[rface(i, j)] = t * (pressure.at(i - 1, j) - pressure.at(i, j));
            }
        }

        for (int j = 0; j < nz; ++j) {
            for (int i = 0; i < nr; ++i) {
                const double divergence = flux_z[zface(i, j)] - flux_z[zface(i, j + 1)] +
                                          flux_r[rface(i, j)] - flux_r[rface(i + 1, j)];
                diag.max_total_velocity_divergence_1_s =
                    std::max(diag.max_total_velocity_divergence_1_s,
                             std::abs(divergence) / g.volume(i));
            }
        }

        last_flux_z = flux_z;
        last_flux_r = flux_r;

        // ---- IMPES saturation, enthalpy and solute transport -------------
        // Face-based and donor-upwinded: each face's water mass is computed
        // once and applied with opposite signs to the two cells it joins, so
        // the discrete balance closes by construction rather than by tolerance.
        const double dt = config.dt_s;
        std::vector<double> face_water_z(static_cast<std::size_t>(nr) *
                                         static_cast<std::size_t>(nz + 1), 0.0);
        std::vector<double> face_solids_z(face_water_z.size(), 0.0);
        std::vector<double> face_enthalpy_z(face_water_z.size(), 0.0);
        std::vector<double> face_water_r(static_cast<std::size_t>(nr + 1) *
                                         static_cast<std::size_t>(nz), 0.0);
        std::vector<double> face_solids_r(face_water_r.size(), 0.0);
        std::vector<double> face_enthalpy_r(face_water_r.size(), 0.0);

        // Solute per unit water, so a face carrying water carries its share.
        const auto donor_concentration = [&](int i, int j) {
            return retained_kg.at(i, j) > kMassEpsilon
                       ? dissolved_kg.at(i, j) / retained_kg.at(i, j)
                       : 0.0;
        };

        for (int j = 0; j <= nz; ++j) {
            for (int i = 0; i < nr; ++i) {
                const double q = flux_z[zface(i, j)];  // positive downward
                if (q == 0.0) continue;
                double f_w = 0.0;
                double donor_t = inlet_temperature_k;
                double donor_c = 0.0;
                if (q > 0.0) {
                    if (j == 0) {
                        f_w = 1.0;  // the screen delivers water, not air
                    } else {
                        f_w = water_fraction[cell(i, j - 1)];
                        donor_t = temperature.at(i, j - 1);
                        donor_c = donor_concentration(i, j - 1);
                    }
                } else {
                    if (j == nz) continue;  // no backflow through the basket
                    f_w = water_fraction[cell(i, j)];
                    donor_t = temperature.at(i, j);
                    donor_c = donor_concentration(i, j);
                }
                const WaterValues donor_values = water_values_at(*water_, donor_t);
                const double mass = q * f_w * donor_values.density_kg_m3 * dt;
                face_water_z[zface(i, j)] = mass;
                face_solids_z[zface(i, j)] = mass * donor_c;
                face_enthalpy_z[zface(i, j)] =
                    mass * donor_values.heat_capacity_j_kg_k * donor_t;
            }
        }
        for (int j = 0; j < nz; ++j) {
            for (int i = 1; i < nr; ++i) {
                const double q = flux_r[rface(i, j)];  // positive outward
                if (q == 0.0) continue;
                const int donor = q > 0.0 ? i - 1 : i;
                const double f_w = water_fraction[cell(donor, j)];
                const double donor_t = temperature.at(donor, j);
                const WaterValues donor_values = water_values_at(*water_, donor_t);
                const double mass = q * f_w * donor_values.density_kg_m3 * dt;
                face_water_r[rface(i, j)] = mass;
                face_solids_r[rface(i, j)] = mass * donor_concentration(donor, j);
                face_enthalpy_r[rface(i, j)] =
                    mass * donor_values.heat_capacity_j_kg_k * donor_t;
            }
        }

        // ---- flux limiters -----------------------------------------------
        // Donor-based limiting, applied to the shared face value so both cells
        // see the same number and the balance still closes. Without this a
        // nearly dry cell exports a pore concentration of dissolved/retained
        // that it does not physically hold, and the solute field diverges.
        {
            std::vector<double> out_water(static_cast<std::size_t>(nr) *
                                          static_cast<std::size_t>(nz), 0.0);
            const auto accumulate_out = [&](int i, int j, double mass) {
                if (mass > 0.0) out_water[cell(i, j)] += mass;
            };
            for (int j = 0; j < nz; ++j) {
                for (int i = 0; i < nr; ++i) {
                    accumulate_out(i, j, face_water_z[zface(i, j + 1)]);
                    if (j > 0) accumulate_out(i, j, -face_water_z[zface(i, j)]);
                    accumulate_out(i, j, face_water_r[rface(i + 1, j)]);
                    accumulate_out(i, j, -face_water_r[rface(i, j)]);
                }
            }

            std::vector<double> water_scale(out_water.size(), 1.0);
            std::vector<double> solute_scale(out_water.size(), 1.0);
            for (int j = 0; j < nz; ++j) {
                for (int i = 0; i < nr; ++i) {
                    const double available = retained_kg.at(i, j);
                    const double leaving = out_water[cell(i, j)];
                    if (leaving > available && leaving > kMassEpsilon) {
                        water_scale[cell(i, j)] = available / leaving;
                    }
                    // Solute leaves at the pore concentration, but never more
                    // than the cell holds.
                    const double carried = leaving * water_scale[cell(i, j)] *
                                           donor_concentration(i, j);
                    if (carried > dissolved_kg.at(i, j) && carried > kMassEpsilon) {
                        solute_scale[cell(i, j)] = dissolved_kg.at(i, j) / carried;
                    }
                }
            }

            const auto rescale = [&](double& water, double& solids, double& enthalpy, int i, int j) {
                const double w = water_scale[cell(i, j)];
                const double c = solute_scale[cell(i, j)];
                water *= w;
                enthalpy *= w;
                solids *= w * c;
            };
            for (int j = 0; j <= nz; ++j) {
                for (int i = 0; i < nr; ++i) {
                    double& water = face_water_z[zface(i, j)];
                    if (water > 0.0 && j > 0) {
                        rescale(water, face_solids_z[zface(i, j)], face_enthalpy_z[zface(i, j)],
                                i, j - 1);
                    } else if (water < 0.0 && j < nz) {
                        rescale(water, face_solids_z[zface(i, j)], face_enthalpy_z[zface(i, j)],
                                i, j);
                    }
                }
            }
            for (int j = 0; j < nz; ++j) {
                for (int i = 1; i < nr; ++i) {
                    double& water = face_water_r[rface(i, j)];
                    const int donor = water > 0.0 ? i - 1 : i;
                    if (water != 0.0) {
                        rescale(water, face_solids_r[rface(i, j)], face_enthalpy_r[rface(i, j)],
                                donor, j);
                    }
                }
            }
        }

        CfdField new_retained = retained_kg;
        CfdField new_dissolved = dissolved_kg;
        CfdField new_temperature = temperature;
        CfdField new_extractable = extractable_kg;
        double inflow_kg = 0.0;
        double outflow_kg = 0.0;
        double outflow_solids_kg = 0.0;
        double outflow_volume_m3 = 0.0;

        for (int i = 0; i < nr; ++i) {
            inflow_kg += face_water_z[zface(i, 0)];
            outflow_kg += face_water_z[zface(i, nz)];
            outflow_solids_kg += face_solids_z[zface(i, nz)];
            const double outlet_density =
                water_values_at(*water_, temperature.at(i, nz - 1)).density_kg_m3;
            outflow_volume_m3 +=
                face_water_z[zface(i, nz)] / std::max(outlet_density, kMassEpsilon);
        }

        bool saturation_invalid = false;
        for (int j = 0; j < nz; ++j) {
            for (int i = 0; i < nr; ++i) {
                const double net_water = face_water_z[zface(i, j)] - face_water_z[zface(i, j + 1)] +
                                         face_water_r[rface(i, j)] - face_water_r[rface(i + 1, j)];
                const double net_solids = face_solids_z[zface(i, j)] -
                                          face_solids_z[zface(i, j + 1)] +
                                          face_solids_r[rface(i, j)] - face_solids_r[rface(i + 1, j)];
                const double net_enthalpy = face_enthalpy_z[zface(i, j)] -
                                            face_enthalpy_z[zface(i, j + 1)] +
                                            face_enthalpy_r[rface(i, j)] -
                                            face_enthalpy_r[rface(i + 1, j)];

                double retained = retained_kg.at(i, j) + net_water;
                double dissolved = dissolved_kg.at(i, j) + net_solids;

                // Enthalpy against the solid matrix, using the advected energy
                // that actually crossed the faces.
                const double cell_dose_kg = recipe.dose_kg * (g.volume(i) / bed_volume_m3);
                const WaterValues cell_values = water_values_at(*water_, temperature.at(i, j));
                const double cp_water = cell_values.heat_capacity_j_kg_k;
                const double thermal_capacity =
                    cell_dose_kg * coeff.coffee_heat_capacity_j_kg_k +
                    std::max(retained_kg.at(i, j), kMassEpsilon) * cp_water;
                const double advected_w =
                    dt > 0.0 ? (net_enthalpy - net_water * cp_water * temperature.at(i, j)) / dt
                             : 0.0;
                const double loss_w = coeff.ambient_heat_loss_w_k * (g.volume(i) / bed_volume_m3) *
                                      (temperature.at(i, j) - coeff.ambient_temperature_k);
                double updated_t =
                    temperature.at(i, j) + (advected_w - loss_w) / thermal_capacity * dt;
                updated_t = std::clamp(updated_t, water_range.min_temperature_k,
                                       water_range.max_temperature_k);

                ShotState view;
                view.puck_temperature_k = updated_t;
                view.liquid_saturation = saturation.at(i, j);
                // coeff.flow_half_saturation_m3_s is calibrated against
                // whole-basket flow, so the local Darcy velocity is scaled back
                // to a basket-equivalent flow. Feeding the raw per-cell flux
                // would make extraction depend on how finely the mesh is cut.
                const double superficial_velocity_m_s =
                    std::max(flux_z[zface(i, j + 1)], 0.0) /
                    std::max(g.axial_area_m2[static_cast<std::size_t>(i)], kMassEpsilon);
                const double equivalent_flow_m3_s =
                    superficial_velocity_m_s * kPi * radius_m * radius_m;
                const double k_ext =
                    extraction_rate_coefficient(view, recipe, coeff, equivalent_flow_m3_s);
                double extracted = k_ext * extractable_kg.at(i, j) * dt;
                extracted = std::clamp(extracted, 0.0, extractable_kg.at(i, j));
                new_extractable.at(i, j) -= extracted;
                dissolved += extracted;

                const double pore_capacity_kg = g.volume(i) * porosity * cell_values.density_kg_m3;
                const double capacity = std::max(pore_capacity_kg, kMassEpsilon);
                // A well-posed IMPES step cannot overfill a cell: as saturation
                // rises the fractional flow rises with it and the cell passes
                // what it takes. Overshoot means the step is too large, so it
                // is a recorded warning rather than a silent truncation.
                if (retained > capacity * (1.0 + 1.0e-9)) {
                    ++diag.saturation_clamp_count;
                    if (config.strict_invariants) {
                        termination = TerminationReason::invalid_state;
                        result.warnings.push_back({"SATURATION_INVARIANT",
                                                   "liquid saturation left [0, 1] beyond tolerance",
                                                   time_s, WarningSeverity::hard});
                        saturation_invalid = true;
                        break;
                    }
                    retained = capacity;
                }
                if (retained < 0.0) {
                    ++diag.saturation_clamp_count;
                    retained = 0.0;
                }
                dissolved = std::max(dissolved, 0.0);

                new_retained.at(i, j) = retained;
                new_dissolved.at(i, j) = dissolved;
                new_temperature.at(i, j) = updated_t;

                const double courant =
                    capacity > kMassEpsilon ? std::abs(net_water) / capacity : 0.0;
                diag.max_courant_number = std::max(diag.max_courant_number, courant);
            }
            if (saturation_invalid) break;
        }
        if (saturation_invalid) {
            break;
        }

        retained_kg = new_retained;
        dissolved_kg = new_dissolved;
        temperature = new_temperature;
        extractable_kg = new_extractable;
        for (int j = 0; j < nz; ++j) {
            for (int i = 0; i < nr; ++i) {
                const double capacity =
                    g.volume(i) * porosity *
                    water_values_at(*water_, temperature.at(i, j)).density_kg_m3;
                saturation.at(i, j) =
                    capacity > kMassEpsilon ? retained_kg.at(i, j) / capacity : 0.0;
            }
        }

        cumulative_water_in_kg += inflow_kg;
        beverage_mass_kg += outflow_kg + outflow_solids_kg;
        water_out_kg += outflow_kg;
        solids_in_cup_kg += outflow_solids_kg;

        time_s = static_cast<double>(step + 1) * dt;
        diag.step_count = step + 1;

        if (time_s + 1.0e-9 >= next_sample_s) {
            double retained_total = 0.0;
            double weighted_t = 0.0;
            double capacity_total = 0.0;
            for (int j = 0; j < nz; ++j) {
                for (int i = 0; i < nr; ++i) {
                    retained_total += retained_kg.at(i, j);
                    const double capacity =
                        g.volume(i) * porosity *
                        water_values_at(*water_, temperature.at(i, j)).density_kg_m3;
                    capacity_total += capacity;
                    weighted_t += temperature.at(i, j) * capacity;
                }
            }
            ShotSample sample;
            sample.time_s = time_s;
            sample.pressure_pa = inlet_pressure_pa;
            sample.inlet_temperature_k = inlet_temperature_k;
            sample.puck_temperature_k =
                capacity_total > kMassEpsilon ? weighted_t / capacity_total : inlet_temperature_k;
            sample.flow_m3_s = dt > 0.0 ? outflow_volume_m3 / dt : 0.0;
            sample.beverage_mass_kg = beverage_mass_kg;
            sample.tds_fraction =
                beverage_mass_kg > kMassEpsilon ? solids_in_cup_kg / beverage_mass_kg : 0.0;
            sample.extraction_yield_fraction =
                recipe.dose_kg > kMassEpsilon ? solids_in_cup_kg / recipe.dose_kg : 0.0;
            sample.saturation =
                capacity_total > kMassEpsilon ? retained_total / capacity_total : 0.0;
            sample.permeability_m2 = absolute_permeability_m2;
            result.samples.push_back(sample);
            next_sample_s += config.sample_interval_s;
        }

        if (recipe.target_beverage_mass_kg.has_value() &&
            beverage_mass_kg >= *recipe.target_beverage_mass_kg) {
            termination = TerminationReason::target_mass_reached;
            break;
        }
        if (time_s >= recipe.maximum_time_s) {
            termination = TerminationReason::time_limit_reached;
            break;
        }
    }

    double retained_total = 0.0;
    double dissolved_total = 0.0;
    double extractable_total = 0.0;
    for (int j = 0; j < nz; ++j) {
        for (int i = 0; i < nr; ++i) {
            retained_total += retained_kg.at(i, j);
            dissolved_total += dissolved_kg.at(i, j);
            extractable_total += extractable_kg.at(i, j);
        }
    }
    diag.water_mass_residual_kg = cumulative_water_in_kg - (retained_total + water_out_kg);
    diag.solids_mass_residual_kg = (initial_extractable_kg - extractable_total) -
                                   (dissolved_total + solids_in_cup_kg);

    result.termination = termination;
    result.elapsed_time_s = time_s;
    result.beverage_mass_kg = beverage_mass_kg;
    result.tds_fraction =
        beverage_mass_kg > kMassEpsilon ? solids_in_cup_kg / beverage_mass_kg : 0.0;
    result.extraction_yield_fraction =
        recipe.dose_kg > kMassEpsilon ? solids_in_cup_kg / recipe.dose_kg : 0.0;

    result.pressure_pa = pressure;
    result.saturation = saturation;
    result.temperature_k = temperature;
    result.pore_tds_fraction = CfdField(nr, nz, 0.0);
    result.axial_velocity_m_s = CfdField(nr, nz, 0.0);
    result.radial_velocity_m_s = CfdField(nr, nz, 0.0);
    for (int j = 0; j < nz; ++j) {
        for (int i = 0; i < nr; ++i) {
            result.pore_tds_fraction.at(i, j) =
                retained_kg.at(i, j) > kMassEpsilon
                    ? dissolved_kg.at(i, j) / retained_kg.at(i, j)
                    : 0.0;
            // Cell-centred superficial (Darcy) velocity, averaged from the two
            // opposing faces. This is a volume-flux-per-area, not a pore
            // velocity: dividing by porosity would give the interstitial speed.
            const double area_z = g.axial_area_m2[static_cast<std::size_t>(i)];
            const std::size_t z_top = static_cast<std::size_t>(j) * static_cast<std::size_t>(nr) +
                                      static_cast<std::size_t>(i);
            const std::size_t z_bottom =
                static_cast<std::size_t>(j + 1) * static_cast<std::size_t>(nr) +
                static_cast<std::size_t>(i);
            result.axial_velocity_m_s.at(i, j) =
                0.5 * (last_flux_z[z_top] + last_flux_z[z_bottom]) / std::max(area_z, kMassEpsilon);

            const std::size_t r_in = static_cast<std::size_t>(j) * (static_cast<std::size_t>(nr) + 1) +
                                     static_cast<std::size_t>(i);
            const std::size_t r_out = r_in + 1;
            const double area_in = g.radial_area_m2[static_cast<std::size_t>(i)];
            const double area_out = g.radial_area_m2[static_cast<std::size_t>(i) + 1];
            const double v_in = area_in > 0.0 ? last_flux_r[r_in] / area_in : 0.0;
            const double v_out = area_out > 0.0 ? last_flux_r[r_out] / area_out : 0.0;
            result.radial_velocity_m_s.at(i, j) = 0.5 * (v_in + v_out);
        }
    }
    if (!all_finite(result)) {
        ValidationResult result_validation;
        result_validation.add("NUMERICAL_FAILURE", "cfd result contained a non-finite value",
                              "cfd.result");
        throw InvalidInputError(result_validation);
    }
    return result;
}

}  // namespace espressolab
