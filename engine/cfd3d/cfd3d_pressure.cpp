#include "cfd3d_pressure.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace espressolab {

double harmonic_mean(double first, double second) {
    if (first <= 0.0 || second <= 0.0) return 0.0;
    return 2.0 * first * second / (first + second);
}

namespace {

constexpr double kMinimumPositive = 1.0e-300;

void finalize_level(PressureLevel& level) {
    level.neighbors.assign(level.diagonal.size(), {});
    for (const PressureEdge& edge : level.edges) {
        level.neighbors[edge.a].push_back({edge.b, edge.transmissibility});
        level.neighbors[edge.b].push_back({edge.a, edge.transmissibility});
    }
}

void add_edge(PressureLevel& level, std::size_t a, std::size_t b, double transmissibility, int axis) {
    if (a == b || transmissibility <= 0.0) return;
    level.edges.push_back({a, b, transmissibility, axis});
    level.diagonal[a] += transmissibility;
    level.diagonal[b] += transmissibility;
}

void add_boundary(PressureLevel& level, std::size_t node, double transmissibility,
                 double boundary_pressure) {
    if (transmissibility <= 0.0) return;
    level.diagonal[node] += transmissibility;
    level.boundary_diagonal[node] += transmissibility;
    level.rhs[node] += transmissibility * boundary_pressure;
}

void apply_operator(const PressureLevel& level, const std::vector<double>& x,
                    std::vector<double>& output) {
    output.assign(level.diagonal.size(), 0.0);
    for (std::size_t i = 0; i < level.diagonal.size(); ++i) {
        output[i] = level.diagonal[i] * x[i];
    }
    for (const PressureEdge& edge : level.edges) {
        output[edge.a] -= edge.transmissibility * x[edge.b];
        output[edge.b] -= edge.transmissibility * x[edge.a];
    }
}

PressureLevel build_coarse_level(PressureLevel& fine) {
    PressureLevel coarse;
    std::map<std::tuple<int, int, int>, std::size_t> coarse_indices;
    fine.fine_to_coarse.resize(fine.coordinates.size());
    for (std::size_t i = 0; i < fine.coordinates.size(); ++i) {
        const std::array<int, 3>& coordinate = fine.coordinates[i];
        const std::tuple<int, int, int> key{coordinate[0] / 2, coordinate[1] / 2,
                                            coordinate[2] / 2};
        const auto found = coarse_indices.find(key);
        std::size_t coarse_index = 0;
        if (found == coarse_indices.end()) {
            coarse_index = coarse.coordinates.size();
            coarse_indices.emplace(key, coarse_index);
            coarse.coordinates.push_back({std::get<0>(key), std::get<1>(key), std::get<2>(key)});
        } else {
            coarse_index = found->second;
        }
        fine.fine_to_coarse[i] = coarse_index;
    }
    coarse.diagonal.assign(coarse.coordinates.size(), 0.0);
    coarse.boundary_diagonal.assign(coarse.coordinates.size(), 0.0);
    coarse.rhs.assign(coarse.coordinates.size(), 0.0);
    for (std::size_t i = 0; i < fine.coordinates.size(); ++i) {
        const std::size_t coarse_index = fine.fine_to_coarse[i];
        coarse.boundary_diagonal[coarse_index] += fine.boundary_diagonal[i];
        coarse.rhs[coarse_index] += fine.rhs[i];
    }
    std::map<std::pair<std::size_t, std::size_t>, double> edge_values;
    for (const PressureEdge& edge : fine.edges) {
        const std::size_t first = fine.fine_to_coarse[edge.a];
        const std::size_t second = fine.fine_to_coarse[edge.b];
        if (first == second) continue;
        const std::pair<std::size_t, std::size_t> key =
            first < second ? std::pair{first, second} : std::pair{second, first};
        edge_values[key] += edge.transmissibility;
    }
    coarse.diagonal = coarse.boundary_diagonal;
    for (const auto& edge : edge_values) {
        add_edge(coarse, edge.first.first, edge.first.second, edge.second, 0);
    }
    finalize_level(coarse);
    return coarse;
}

struct GeometricMultigrid {
    std::vector<PressureLevel> levels;
    struct Workspace {
        std::vector<double> applied;
        std::vector<double> residual;
        std::vector<double> coarse_rhs;
        std::vector<double> coarse_correction;
    };
    std::vector<Workspace> workspace;

    explicit GeometricMultigrid(PressureLevel fine) {
        levels.reserve(16);
        levels.push_back(std::move(fine));
        while (levels.back().coordinates.size() > 1U) {
            PressureLevel coarse = build_coarse_level(levels.back());
            if (coarse.coordinates.size() >= levels.back().coordinates.size()) break;
            levels.push_back(std::move(coarse));
        }
        workspace.resize(levels.size());
    }
};

long double dot_product(const std::vector<double>& first, const std::vector<double>& second) {
    long double result = 0.0L;
    for (std::size_t i = 0; i < first.size(); ++i) {
        result += static_cast<long double>(first[i]) * static_cast<long double>(second[i]);
    }
    return result;
}

double vector_norm(const std::vector<double>& values) {
    return static_cast<double>(std::sqrt(std::max(0.0L, dot_product(values, values))));
}

void gauss_seidel_sweep(const PressureLevel& level, const std::vector<double>& rhs,
                        std::vector<double>& x, bool reverse) {
    if (!reverse) {
        for (std::size_t i = 0; i < level.diagonal.size(); ++i) {
            double remaining = rhs[i];
            for (const auto& neighbor : level.neighbors[i]) {
                remaining -= neighbor.second * x[neighbor.first];
            }
            x[i] = remaining / level.diagonal[i];
        }
    } else {
        for (std::size_t count = level.diagonal.size(); count > 0; --count) {
            const std::size_t i = count - 1U;
            double remaining = rhs[i];
            for (const auto& neighbor : level.neighbors[i]) {
                remaining -= neighbor.second * x[neighbor.first];
            }
            x[i] = remaining / level.diagonal[i];
        }
    }
}

void symmetric_smoothing(const PressureLevel& level, const std::vector<double>& rhs,
                         std::vector<double>& x) {
    gauss_seidel_sweep(level, rhs, x, false);
    gauss_seidel_sweep(level, rhs, x, true);
}

bool solve_coarse_system(const PressureLevel& level, const std::vector<double>& rhs,
                         std::vector<double>& x) {
    const std::size_t count = level.diagonal.size();
    std::vector<double> lower(count * count, 0.0);
    for (std::size_t i = 0; i < count; ++i) lower[i * count + i] = level.diagonal[i];
    for (const PressureEdge& edge : level.edges) {
        lower[edge.a * count + edge.b] -= edge.transmissibility;
        lower[edge.b * count + edge.a] -= edge.transmissibility;
    }

    // The coarse operator is a symmetric positive-definite Galerkin operator.
    // An exact Cholesky solve avoids making the V-cycle depend on a finite
    // number of coarse smoothing sweeps when material jumps are large.
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = 0; j <= i; ++j) {
            long double value = static_cast<long double>(lower[i * count + j]);
            for (std::size_t k = 0; k < j; ++k) {
                value -= static_cast<long double>(lower[i * count + k]) *
                         static_cast<long double>(lower[j * count + k]);
            }
            if (i == j) {
                if (!std::isfinite(static_cast<double>(value)) || value <= 0.0L) return false;
                lower[i * count + j] = static_cast<double>(std::sqrt(value));
            } else {
                lower[i * count + j] = static_cast<double>(value) /
                                       lower[j * count + j];
            }
        }
    }

    std::vector<double> forward(count, 0.0);
    for (std::size_t i = 0; i < count; ++i) {
        long double value = static_cast<long double>(rhs[i]);
        for (std::size_t k = 0; k < i; ++k) {
            value -= static_cast<long double>(lower[i * count + k]) *
                     static_cast<long double>(forward[k]);
        }
        forward[i] = static_cast<double>(value) / lower[i * count + i];
    }
    x.assign(count, 0.0);
    for (std::size_t remaining = count; remaining > 0; --remaining) {
        const std::size_t i = remaining - 1U;
        long double value = static_cast<long double>(forward[i]);
        for (std::size_t k = i + 1U; k < count; ++k) {
            value -= static_cast<long double>(lower[k * count + i]) *
                     static_cast<long double>(x[k]);
        }
        x[i] = static_cast<double>(value) / lower[i * count + i];
    }
    return true;
}

bool multigrid_cycle(const std::vector<PressureLevel>& levels,
                     std::vector<GeometricMultigrid::Workspace>& workspace,
                     std::size_t level_index, const std::vector<double>& rhs,
                     std::vector<double>& x) {
    const PressureLevel& level = levels[level_index];
    if (level_index + 1U == levels.size()) {
        return solve_coarse_system(level, rhs, x);
    }

    symmetric_smoothing(level, rhs, x);
    symmetric_smoothing(level, rhs, x);
    GeometricMultigrid::Workspace& current = workspace[level_index];
    current.applied.resize(rhs.size());
    current.residual.resize(rhs.size());
    apply_operator(level, x, current.applied);
    for (std::size_t i = 0; i < rhs.size(); ++i) {
        current.residual[i] = rhs[i] - current.applied[i];
    }

    const PressureLevel& coarse = levels[level_index + 1U];
    current.coarse_rhs.assign(coarse.diagonal.size(), 0.0);
    for (std::size_t i = 0; i < current.residual.size(); ++i) {
        current.coarse_rhs[level.fine_to_coarse[i]] += current.residual[i];
    }
    current.coarse_correction.assign(current.coarse_rhs.size(), 0.0);
    if (!multigrid_cycle(levels, workspace, level_index + 1U, current.coarse_rhs,
                         current.coarse_correction)) {
        return false;
    }
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] += current.coarse_correction[level.fine_to_coarse[i]];
    }
    symmetric_smoothing(level, rhs, x);
    symmetric_smoothing(level, rhs, x);
    return true;
}

bool apply_preconditioner(GeometricMultigrid& multigrid, const std::vector<double>& rhs,
                          std::vector<double>& output) {
    output.assign(rhs.size(), 0.0);
    if (!multigrid_cycle(multigrid.levels, multigrid.workspace, 0, rhs, output)) {
        return false;
    }
    const PressureLevel& fine = multigrid.levels.front();
    for (std::size_t i = 0; i < output.size(); ++i) {
        if (fine.diagonal[i] <= 0.0) return false;
        output[i] += rhs[i] / fine.diagonal[i];
    }
    return true;
}

bool solve_uniform_layers(const PressureLevel& system, std::vector<double>& pressure) {
    const std::size_t layers = system.layer_mobility.size();
    if (layers == 0U || system.layer_area_m2 <= 0.0 || system.axial_spacing_m <= 0.0) return false;
    std::vector<double> diagonal(layers, 0.0);
    std::vector<double> lower(layers, 0.0);
    std::vector<double> upper(layers, 0.0);
    std::vector<double> rhs(layers, 0.0);
    for (std::size_t layer = 0; layer < layers; ++layer) {
        const double mobility = system.layer_mobility[layer];
        if (!std::isfinite(mobility) || mobility <= 0.0) return false;
        const double top = layer == 0U
                               ? mobility * system.layer_area_m2 /
                                     (0.5 * system.axial_spacing_m)
                               : harmonic_mean(system.layer_mobility[layer - 1U], mobility) *
                                     system.layer_area_m2 / system.axial_spacing_m;
        const double bottom = layer + 1U == layers
                                  ? mobility * system.layer_area_m2 /
                                        (0.5 * system.axial_spacing_m)
                                  : harmonic_mean(mobility, system.layer_mobility[layer + 1U]) *
                                        system.layer_area_m2 / system.axial_spacing_m;
        diagonal[layer] = top + bottom;
        if (layer > 0U) lower[layer] = -top;
        if (layer + 1U < layers) upper[layer] = -bottom;
        if (layer == 0U) rhs[layer] += top * system.inlet_pressure_pa;
        if (layer + 1U == layers) rhs[layer] += bottom * system.outlet_pressure_pa;
    }

    for (std::size_t layer = 1U; layer < layers; ++layer) {
        if (std::abs(diagonal[layer - 1U]) <= kMinimumPositive) return false;
        const double factor = lower[layer] / diagonal[layer - 1U];
        diagonal[layer] -= factor * upper[layer - 1U];
        rhs[layer] -= factor * rhs[layer - 1U];
    }
    std::vector<double> solution(layers, 0.0);
    if (std::abs(diagonal.back()) <= kMinimumPositive) return false;
    solution.back() = rhs.back() / diagonal.back();
    for (std::size_t remaining = layers - 1U; remaining > 0U; --remaining) {
        const std::size_t layer = remaining - 1U;
        if (std::abs(diagonal[layer]) <= kMinimumPositive) return false;
        solution[layer] = (rhs[layer] - upper[layer] * solution[layer + 1U]) / diagonal[layer];
    }

    const std::size_t active_count = pressure.size() / layers;
    for (std::size_t layer = 0; layer < layers; ++layer) {
        for (std::size_t active = 0; active < active_count; ++active) {
            pressure[node_index(active, static_cast<int>(layer), active_count)] = solution[layer];
        }
    }
    return true;
}

}  // namespace

PressureLevel build_fine_pressure_level(const GeometryInternal& geometry,
                                        const std::vector<double>& mobility,
                                        double inlet_pressure_pa, double outlet_pressure_pa) {
    const Cfd3dMesh& mesh = geometry.public_geometry.mesh;
    const std::size_t active_count = geometry.active_roots.size();
    const std::size_t node_count = active_count * static_cast<std::size_t>(mesh.nz);
    PressureLevel level;
    level.diagonal.assign(node_count, 0.0);
    level.boundary_diagonal.assign(node_count, 0.0);
    level.rhs.assign(node_count, 0.0);
    level.layer_mobility.assign(static_cast<std::size_t>(mesh.nz), 0.0);
    level.layer_area_m2 = 0.0;
    for (const double area : geometry.aggregate_area_xy_m2) level.layer_area_m2 += area;
    level.axial_spacing_m = geometry.public_geometry.dz_m;
    level.inlet_pressure_pa = inlet_pressure_pa;
    level.outlet_pressure_pa = outlet_pressure_pa;
    level.uniform_xy_mobility = true;
    level.coordinates.reserve(node_count);
    for (int z = 0; z < mesh.nz; ++z) {
        level.layer_mobility[static_cast<std::size_t>(z)] =
            mobility[node_index(0, z, active_count)];
        for (std::size_t active = 0; active < active_count; ++active) {
            const double layer_value = level.layer_mobility[static_cast<std::size_t>(z)];
            const double value = mobility[node_index(active, z, active_count)];
            const double scale = std::max({1.0, std::abs(layer_value), std::abs(value)});
            if (std::abs(layer_value - value) > 1.0e-14 * scale) {
                level.uniform_xy_mobility = false;
            }
            const int root = geometry.active_roots[active];
            const int x = root % mesh.nx;
            const int y = root / mesh.nx;
            level.coordinates.push_back({x, y, z});
        }
    }

    const auto root_at = [&](int x, int y) {
        return geometry.root_for_xy[xy_index(x, y, mesh.nx)];
    };
    const auto active_at_root = [&](int root) {
        return root < 0
                   ? -1
                   : geometry.active_index_for_root[static_cast<std::size_t>(root)];
    };

    if (!level.uniform_xy_mobility) {
        for (int z = 0; z < mesh.nz; ++z) {
            for (int y = 0; y < mesh.ny; ++y) {
                for (int x = 0; x + 1 < mesh.nx; ++x) {
                    const int left_root = root_at(x, y);
                    const int right_root = root_at(x + 1, y);
                    const int left = active_at_root(left_root);
                    const int right = active_at_root(right_root);
                    if (left < 0 || right < 0 || left == right) continue;
                    const double aperture = geometry.public_geometry.x_face_aperture_m[
                        static_cast<std::size_t>(x + 1) +
                        (static_cast<std::size_t>(mesh.nx) + 1U) * static_cast<std::size_t>(y)];
                    const double transmissibility =
                        harmonic_mean(mobility[node_index(static_cast<std::size_t>(left), z, active_count)],
                                      mobility[node_index(static_cast<std::size_t>(right), z, active_count)]) *
                        aperture * geometry.public_geometry.dz_m / geometry.public_geometry.dx_m;
                    add_edge(level, node_index(static_cast<std::size_t>(left), z, active_count),
                             node_index(static_cast<std::size_t>(right), z, active_count),
                             transmissibility, 0);
                }
            }
        }
        for (int z = 0; z < mesh.nz; ++z) {
            for (int y = 0; y + 1 < mesh.ny; ++y) {
                for (int x = 0; x < mesh.nx; ++x) {
                    const int lower_root = root_at(x, y);
                    const int upper_root = root_at(x, y + 1);
                    const int lower = active_at_root(lower_root);
                    const int upper = active_at_root(upper_root);
                    if (lower < 0 || upper < 0 || lower == upper) continue;
                    const double aperture = geometry.public_geometry.y_face_aperture_m[
                        static_cast<std::size_t>(x) + static_cast<std::size_t>(mesh.nx) *
                                                         static_cast<std::size_t>(y + 1)];
                    const double transmissibility =
                        harmonic_mean(mobility[node_index(static_cast<std::size_t>(lower), z, active_count)],
                                      mobility[node_index(static_cast<std::size_t>(upper), z, active_count)]) *
                        aperture * geometry.public_geometry.dz_m / geometry.public_geometry.dy_m;
                    add_edge(level, node_index(static_cast<std::size_t>(lower), z, active_count),
                             node_index(static_cast<std::size_t>(upper), z, active_count),
                             transmissibility, 1);
                }
            }
        }
    }
    for (int z = 0; z + 1 < mesh.nz; ++z) {
        for (std::size_t active = 0; active < active_count; ++active) {
            const double area = geometry.aggregate_area_xy_m2[active];
            const double transmissibility =
                harmonic_mean(mobility[node_index(active, z, active_count)],
                              mobility[node_index(active, z + 1, active_count)]) *
                area / geometry.public_geometry.dz_m;
            add_edge(level, node_index(active, z, active_count),
                     node_index(active, z + 1, active_count), transmissibility, 2);
        }
    }
    for (int z = 0; z < mesh.nz; ++z) {
        for (std::size_t active = 0; active < active_count; ++active) {
            const std::size_t node = node_index(active, z, active_count);
            const double area = geometry.aggregate_area_xy_m2[active];
            const double coefficient = mobility[node] * area / (0.5 * geometry.public_geometry.dz_m);
            if (z == 0) add_boundary(level, node, coefficient, inlet_pressure_pa);
            if (z + 1 == mesh.nz) add_boundary(level, node, coefficient, outlet_pressure_pa);
        }
    }
    finalize_level(level);
    return level;
}

PressureSolveResult solve_pressure(const PressureLevel& system, std::vector<double>& pressure,
                                   double tolerance, int maximum_iterations,
                                   const CancellationCallback& is_cancelled) {
    std::vector<double> applied;
    apply_operator(system, pressure, applied);
    std::vector<double> residual(system.rhs.size(), 0.0);
    for (std::size_t i = 0; i < residual.size(); ++i) residual[i] = system.rhs[i] - applied[i];

    const double rhs_norm = vector_norm(system.rhs);
    const double scale = std::max(rhs_norm, kMinimumPositive);
    PressureSolveResult result;
    result.residual = vector_norm(residual) / scale;
    if (!std::isfinite(result.residual)) return result;
    if (system.uniform_xy_mobility) {
        result.converged = solve_uniform_layers(system, pressure);
        if (!result.converged) return result;
        apply_operator(system, pressure, applied);
        for (std::size_t i = 0; i < residual.size(); ++i) residual[i] = system.rhs[i] - applied[i];
        result.residual = vector_norm(residual) / scale;
        result.converged = std::isfinite(result.residual) && result.residual <= tolerance;
        return result;
    }

    GeometricMultigrid multigrid(system);
    if (result.residual <= tolerance) {
        result.converged = true;
        return result;
    }

    std::vector<double> direction;
    if (!apply_preconditioner(multigrid, residual, direction)) return result;
    const long double initial_rho = dot_product(residual, direction);
    if (!std::isfinite(static_cast<double>(initial_rho)) || initial_rho <= 0.0L) return result;
    long double rho = initial_rho;
    std::vector<double> search = direction;
    std::vector<double> matrix_search;
    for (int iteration = 1; iteration <= maximum_iterations; ++iteration) {
        if ((iteration & 31) == 1) throw_if_cancelled(is_cancelled);
        apply_operator(system, search, matrix_search);
        const long double denominator = dot_product(search, matrix_search);
        if (!std::isfinite(static_cast<double>(denominator)) || denominator <= 0.0L) {
            result.iterations = iteration - 1;
            return result;
        }
        const double alpha = static_cast<double>(rho / denominator);
        for (std::size_t i = 0; i < pressure.size(); ++i) {
            pressure[i] += alpha * search[i];
            residual[i] -= alpha * matrix_search[i];
        }
        result.iterations = iteration;
        if (iteration % 25 == 0) {
            apply_operator(system, pressure, applied);
            for (std::size_t i = 0; i < residual.size(); ++i) {
                residual[i] = system.rhs[i] - applied[i];
            }
        }
        result.residual = vector_norm(residual) / scale;
        if (!std::isfinite(result.residual)) return result;
        if (result.residual <= tolerance) {
            result.converged = true;
            return result;
        }
        if (!apply_preconditioner(multigrid, residual, direction)) return result;
        const long double next_rho = dot_product(residual, direction);
        if (!std::isfinite(static_cast<double>(next_rho)) || next_rho <= 0.0L) return result;
        const double beta = static_cast<double>(next_rho / rho);
        for (std::size_t i = 0; i < search.size(); ++i) {
            search[i] = direction[i] + beta * search[i];
        }
        rho = next_rho;
    }
    return result;
}

}  // namespace espressolab
