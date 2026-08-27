#include "espressolab/cfd3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "espressolab/extraction.hpp"
#include "espressolab/puck.hpp"
#include "espressolab/simulator.hpp"
#include "espressolab/units.hpp"
#include "espressolab/version.hpp"

namespace espressolab {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kGeometryRelativeTolerance = 1.0e-12;
constexpr double kSliverFraction = 0.05;
constexpr double kMassEpsilon = 1.0e-12;
constexpr double kAirViscosityPaS = 1.85e-5;
constexpr double kMinimumPositive = 1.0e-300;
constexpr std::size_t kInvalidNode = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kMaximumSnapshots = 128;
constexpr std::uint64_t kMaximumSnapshotBytes = 1ULL << 30;
constexpr std::size_t kSnapshotFieldCount = 7;

struct Point {
    double x = 0.0;
    double y = 0.0;
};

double cross(const Point& a, const Point& b) { return a.x * b.y - a.y * b.x; }

double dot(const Point& a, const Point& b) { return a.x * b.x + a.y * b.y; }

Point operator+(const Point& a, const Point& b) { return {a.x + b.x, a.y + b.y}; }

Point operator-(const Point& a, const Point& b) { return {a.x - b.x, a.y - b.y}; }

Point operator*(const Point& a, double factor) { return {a.x * factor, a.y * factor}; }

double squared_norm(const Point& point) { return dot(point, point); }

std::size_t field_size(int nx, int ny, int nz) {
    if (nx < 0 || ny < 0 || nz < 0) {
        throw std::invalid_argument("Cfd3dField dimensions must not be negative");
    }
    return static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny) *
           static_cast<std::size_t>(nz);
}

double circle_triangle_intersection_area(const Point& a, const Point& b, double radius_m) {
    const Point delta = b - a;
    const double quadratic = dot(delta, delta);
    std::vector<double> cuts{0.0, 1.0};
    if (quadratic > 0.0) {
        const double linear = 2.0 * dot(a, delta);
        const double constant = squared_norm(a) - radius_m * radius_m;
        const double discriminant = linear * linear - 4.0 * quadratic * constant;
        const double discriminant_tolerance =
            kGeometryRelativeTolerance * std::max(radius_m * radius_m, quadratic);
        if (discriminant >= -discriminant_tolerance) {
            const double root = std::sqrt(std::max(0.0, discriminant));
            const double denominator = 2.0 * quadratic;
            const double first = (-linear - root) / denominator;
            const double second = (-linear + root) / denominator;
            if (first > 0.0 && first < 1.0) cuts.push_back(first);
            if (second > 0.0 && second < 1.0) cuts.push_back(second);
        }
    }
    std::sort(cuts.begin(), cuts.end());

    double area = 0.0;
    const double radius_squared = radius_m * radius_m;
    const double point_tolerance =
        kGeometryRelativeTolerance * std::max(radius_squared, quadratic);
    for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
        const double t0 = cuts[i];
        const double t1 = cuts[i + 1];
        if (t1 <= t0) continue;
        const Point first = a + delta * t0;
        const Point second = a + delta * t1;
        const Point midpoint = a + delta * ((t0 + t1) * 0.5);
        if (squared_norm(midpoint) <= radius_squared + point_tolerance) {
            area += 0.5 * cross(first, second);
        } else {
            area += 0.5 * radius_squared * std::atan2(cross(first, second), dot(first, second));
        }
    }
    return area;
}

double circle_rectangle_area(double x0, double x1, double y0, double y1, double radius_m,
                             double full_area_m2) {
    const std::array<Point, 4> corners{{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}};
    double signed_area = 0.0;
    for (std::size_t i = 0; i < corners.size(); ++i) {
        signed_area += circle_triangle_intersection_area(
            corners[i], corners[(i + 1) % corners.size()], radius_m);
    }
    const double area = std::abs(signed_area);
    const double tolerance =
        kGeometryRelativeTolerance * std::max(full_area_m2, kPi * radius_m * radius_m);
    if (area <= tolerance) return 0.0;
    if (full_area_m2 - area <= tolerance) return full_area_m2;
    return std::clamp(area, 0.0, full_area_m2);
}

double circle_segment_aperture(double fixed_coordinate, double low, double high,
                               double radius_m) {
    const double radius_squared = radius_m * radius_m;
    const double coordinate_tolerance =
        kGeometryRelativeTolerance * std::max(radius_m, std::abs(fixed_coordinate));
    double coordinate = fixed_coordinate;
    if (std::abs(std::abs(coordinate) - radius_m) <= coordinate_tolerance) coordinate = radius_m;
    const double radicand = radius_squared - coordinate * coordinate;
    const double radicand_tolerance = kGeometryRelativeTolerance * radius_squared;
    if (radicand <= radicand_tolerance) return 0.0;
    const double extent = std::sqrt(std::max(0.0, radicand));
    const double clipped_low = std::max(low, -extent);
    const double clipped_high = std::min(high, extent);
    const double length = std::max(0.0, clipped_high - clipped_low);
    const double full_length = high - low;
    const double length_tolerance =
        kGeometryRelativeTolerance * std::max(full_length, 2.0 * radius_m);
    if (length <= length_tolerance) return 0.0;
    if (full_length - length <= length_tolerance) return full_length;
    return length;
}

struct GeometryInternal {
    Cfd3dGeometry public_geometry;
    std::vector<int> root_for_xy;
    std::vector<int> active_roots;
    std::vector<int> active_index_for_root;
    std::vector<double> aggregate_area_xy_m2;
};

std::size_t xy_index(int x, int y, int nx) {
    return static_cast<std::size_t>(x) + static_cast<std::size_t>(nx) * static_cast<std::size_t>(y);
}

GeometryInternal build_geometry(double radius_m, double depth_m, const Cfd3dMesh& mesh) {
    GeometryInternal geometry;
    Cfd3dGeometry& output = geometry.public_geometry;
    output.mesh = mesh;
    output.x_min_m = -radius_m;
    output.y_min_m = -radius_m;
    output.dx_m = 2.0 * radius_m / static_cast<double>(mesh.nx);
    output.dy_m = 2.0 * radius_m / static_cast<double>(mesh.ny);
    output.dz_m = depth_m / static_cast<double>(mesh.nz);

    const std::size_t xy_count = static_cast<std::size_t>(mesh.nx) * static_cast<std::size_t>(mesh.ny);
    output.cell_area_xy_m2.assign(xy_count, 0.0);
    output.effective_cell_area_xy_m2.assign(xy_count, 0.0);
    output.agglomerate_parent.assign(xy_count, -1);
    output.classification.assign(xy_count, Cfd3dCellClassification::outside);
    output.x_face_aperture_m.assign(
        (static_cast<std::size_t>(mesh.nx) + 1U) * static_cast<std::size_t>(mesh.ny), 0.0);
    output.y_face_aperture_m.assign(
        (static_cast<std::size_t>(mesh.ny) + 1U) * static_cast<std::size_t>(mesh.nx), 0.0);

    const double full_cell_area = output.dx_m * output.dy_m;
    for (int y = 0; y < mesh.ny; ++y) {
        const double y0 = output.y_min_m + static_cast<double>(y) * output.dy_m;
        const double y1 = y0 + output.dy_m;
        for (int x = 0; x < mesh.nx; ++x) {
            const double x0 = output.x_min_m + static_cast<double>(x) * output.dx_m;
            const double x1 = x0 + output.dx_m;
            const std::size_t index = xy_index(x, y, mesh.nx);
            const double area =
                circle_rectangle_area(x0, x1, y0, y1, radius_m, full_cell_area);
            output.cell_area_xy_m2[index] = area;
            if (area == 0.0) {
                output.classification[index] = Cfd3dCellClassification::outside;
            } else if (area == full_cell_area) {
                output.classification[index] = Cfd3dCellClassification::inside;
            } else {
                output.classification[index] = Cfd3dCellClassification::cut;
            }
        }
    }

    for (int y = 0; y < mesh.ny; ++y) {
        const double y0 = output.y_min_m + static_cast<double>(y) * output.dy_m;
        const double y1 = y0 + output.dy_m;
        for (int x = 0; x <= mesh.nx; ++x) {
            const double coordinate = output.x_min_m + static_cast<double>(x) * output.dx_m;
            output.x_face_aperture_m[static_cast<std::size_t>(x) +
                                     (static_cast<std::size_t>(mesh.nx) + 1U) *
                                         static_cast<std::size_t>(y)] =
                circle_segment_aperture(coordinate, y0, y1, radius_m);
        }
    }
    for (int y = 0; y <= mesh.ny; ++y) {
        const double coordinate = output.y_min_m + static_cast<double>(y) * output.dy_m;
        for (int x = 0; x < mesh.nx; ++x) {
            const double x0 = output.x_min_m + static_cast<double>(x) * output.dx_m;
            const double x1 = x0 + output.dx_m;
            output.y_face_aperture_m[static_cast<std::size_t>(x) +
                                     static_cast<std::size_t>(mesh.nx) * static_cast<std::size_t>(y)] =
                circle_segment_aperture(coordinate, x0, x1, radius_m);
        }
    }

    geometry.root_for_xy.assign(xy_count, -1);
    std::vector<bool> sliver(xy_count, false);
    for (std::size_t index = 0; index < xy_count; ++index) {
        if (output.cell_area_xy_m2[index] > 0.0) {
            geometry.root_for_xy[index] = static_cast<int>(index);
            sliver[index] = output.cell_area_xy_m2[index] < kSliverFraction * full_cell_area;
            if (sliver[index]) {
                output.classification[index] = Cfd3dCellClassification::agglomerated;
            }
        }
    }

    struct Candidate {
        int index = -1;
        double aperture = 0.0;
    };
    const auto add_candidate = [&](std::vector<Candidate>& candidates, int x, int y, int nx,
                                   int ny, double aperture) {
        if (x < 0 || x >= nx || y < 0 || y >= ny) return;
        const std::size_t candidate_index = xy_index(x, y, nx);
        if (geometry.root_for_xy[candidate_index] < 0 || aperture <= 0.0) return;
        candidates.push_back({static_cast<int>(candidate_index), aperture});
    };

    for (std::size_t index = 0; index < xy_count; ++index) {
        if (!sliver[index]) continue;
        const int x = static_cast<int>(index % static_cast<std::size_t>(mesh.nx));
        const int y = static_cast<int>(index / static_cast<std::size_t>(mesh.nx));
        std::vector<Candidate> candidates;
        if (x > 0) {
            const double aperture = output.x_face_aperture_m[static_cast<std::size_t>(x) +
                                                              (static_cast<std::size_t>(mesh.nx) + 1U) *
                                                                  static_cast<std::size_t>(y)];
            add_candidate(candidates, x - 1, y, mesh.nx, mesh.ny, aperture);
        }
        if (x + 1 < mesh.nx) {
            const double aperture = output.x_face_aperture_m[static_cast<std::size_t>(x + 1) +
                                                              (static_cast<std::size_t>(mesh.nx) + 1U) *
                                                                  static_cast<std::size_t>(y)];
            add_candidate(candidates, x + 1, y, mesh.nx, mesh.ny, aperture);
        }
        if (y > 0) {
            const double aperture = output.y_face_aperture_m[static_cast<std::size_t>(x) +
                                                              static_cast<std::size_t>(mesh.nx) *
                                                                  static_cast<std::size_t>(y)];
            add_candidate(candidates, x, y - 1, mesh.nx, mesh.ny, aperture);
        }
        if (y + 1 < mesh.ny) {
            const double aperture = output.y_face_aperture_m[static_cast<std::size_t>(x) +
                                                              static_cast<std::size_t>(mesh.nx) *
                                                                  static_cast<std::size_t>(y + 1)];
            add_candidate(candidates, x, y + 1, mesh.nx, mesh.ny, aperture);
        }

        std::stable_sort(candidates.begin(), candidates.end(),
                         [&](const Candidate& first, const Candidate& second) {
                             if (first.aperture != second.aperture) {
                                 return first.aperture > second.aperture;
                             }
                             return first.index < second.index;
                         });
        int selected = -1;
        for (const Candidate& candidate : candidates) {
            if (!sliver[static_cast<std::size_t>(candidate.index)]) {
                selected = candidate.index;
                break;
            }
        }
        if (selected < 0 && !candidates.empty()) selected = candidates.front().index;
        if (selected < 0) {
            ValidationResult validation;
            validation.add("NONPHYSICAL_INPUT",
                           "cfd3d geometry contains a sliver with no positive adjacent cell",
                           "cfd3d.mesh");
            throw InvalidInputError(validation);
        }
        geometry.root_for_xy[index] = selected;
    }

    // Slivers choose a non-sliver neighbor whenever possible, so one pass is
    // enough and the parent map is independent of container/hash iteration order.
    geometry.active_index_for_root.assign(xy_count, -1);
    for (std::size_t index = 0; index < xy_count; ++index) {
        if (geometry.root_for_xy[index] == static_cast<int>(index)) {
            geometry.active_index_for_root[index] = static_cast<int>(geometry.active_roots.size());
            geometry.active_roots.push_back(static_cast<int>(index));
        }
    }
    geometry.aggregate_area_xy_m2.assign(geometry.active_roots.size(), 0.0);
    for (std::size_t index = 0; index < xy_count; ++index) {
        const int root = geometry.root_for_xy[index];
        if (root >= 0) {
            geometry.aggregate_area_xy_m2[static_cast<std::size_t>(
                geometry.active_index_for_root[static_cast<std::size_t>(root)])] +=
                output.cell_area_xy_m2[index];
            output.agglomerate_parent[index] = root;
        }
    }
    for (std::size_t index = 0; index < xy_count; ++index) {
        const int root = geometry.root_for_xy[index];
        if (root >= 0) {
            output.effective_cell_area_xy_m2[index] =
                geometry.aggregate_area_xy_m2[static_cast<std::size_t>(
                    geometry.active_index_for_root[static_cast<std::size_t>(root)])];
        }
    }
    return geometry;
}

bool material_is_uniform_in_xy(const GeometryInternal& geometry,
                               const Cfd3dMaterialField& material) {
    if (material.empty()) return true;
    const Cfd3dMesh& mesh = geometry.public_geometry.mesh;
    for (int z = 0; z < mesh.nz; ++z) {
        double reference = 0.0;
        bool reference_set = false;
        for (std::size_t xy = 0; xy < geometry.root_for_xy.size(); ++xy) {
            if (geometry.root_for_xy[xy] < 0) continue;
            const int x = static_cast<int>(xy % static_cast<std::size_t>(mesh.nx));
            const int y = static_cast<int>(xy / static_cast<std::size_t>(mesh.nx));
            const double value = material.at(x, y, z);
            if (!reference_set) {
                reference = value;
                reference_set = true;
            }
            const double scale = std::max({1.0, std::abs(reference), std::abs(value)});
            if (std::abs(reference - value) > 1.0e-14 * scale) return false;
        }
    }
    return true;
}

void collapse_uniform_xy_state(GeometryInternal& geometry) {
    if (geometry.active_roots.empty()) return;
    const int representative = geometry.active_roots.front();
    double total_area = 0.0;
    for (const double area : geometry.public_geometry.cell_area_xy_m2) total_area += area;

    for (std::size_t xy = 0; xy < geometry.root_for_xy.size(); ++xy) {
        if (geometry.public_geometry.cell_area_xy_m2[xy] <= 0.0) {
            geometry.root_for_xy[xy] = -1;
        } else {
            geometry.root_for_xy[xy] = representative;
        }
    }
    geometry.active_roots.assign(1, representative);
    geometry.active_index_for_root.assign(geometry.root_for_xy.size(), -1);
    geometry.active_index_for_root[static_cast<std::size_t>(representative)] = 0;
    geometry.aggregate_area_xy_m2.assign(1, total_area);
}

double harmonic_mean(double first, double second) {
    if (first <= 0.0 || second <= 0.0) return 0.0;
    return 2.0 * first * second / (first + second);
}

struct PressureEdge {
    std::size_t a = 0;
    std::size_t b = 0;
    double transmissibility = 0.0;
    int axis = 0;
};

struct PressureLevel {
    std::vector<PressureEdge> edges;
    std::vector<double> diagonal;
    std::vector<double> boundary_diagonal;
    std::vector<double> rhs;
    std::vector<std::vector<std::pair<std::size_t, double>>> neighbors;
    std::vector<std::array<int, 3>> coordinates;
    std::vector<std::size_t> fine_to_coarse;
    bool uniform_xy_mobility = false;
    std::vector<double> layer_mobility;
    double layer_area_m2 = 0.0;
    double axial_spacing_m = 0.0;
    double inlet_pressure_pa = 0.0;
    double outlet_pressure_pa = 0.0;
};

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

std::size_t node_index(std::size_t active_xy, int z, std::size_t active_count) {
    return active_xy + active_count * static_cast<std::size_t>(z);
}

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

struct PressureSolveResult {
    bool converged = false;
    int iterations = 0;
    double residual = std::numeric_limits<double>::infinity();
};

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

double node_capacity(const GeometryInternal& geometry, std::size_t active, double temperature_k,
                     double porosity, const WaterProperties& water) {
    return geometry.aggregate_area_xy_m2[active] * geometry.public_geometry.dz_m * porosity *
           water.density_kg_m3(temperature_k);
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
                                     const WaterProperties& water,
                                     const ModelCoefficients& coeff) {
    const std::size_t count = state.retained_water_kg.size();
    const std::size_t active_count = geometry.active_roots.size();
    std::vector<double> mobility(count, 0.0);
    for (std::size_t node = 0; node < count; ++node) {
        const std::size_t active = node % active_count;
        const double capacity = node_capacity(geometry, active, state.temperature_k[node], porosity,
                                              water);
        const double saturation = capacity > kMassEpsilon
                                      ? state.retained_water_kg[node] / capacity
                                      : 0.0;
        const double clamped_saturation = std::clamp(saturation, 0.0, 1.0);
        const double permeability = absolute_permeability_m2 * material[node];
        const double viscosity = water.viscosity_pa_s(state.temperature_k[node]);
        const double water_mobility =
            permeability * wetting_factor(clamped_saturation, coeff.dry_permeability_multiplier) /
            viscosity;
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
    if (mesh.nx < 1 || mesh.nx > 128) {
        validation.add("NONPHYSICAL_INPUT", "cfd3d mesh.nx must be between 1 and 128",
                       "cfd3d.mesh.nx");
    }
    if (mesh.ny < 1 || mesh.ny > 128) {
        validation.add("NONPHYSICAL_INPUT", "cfd3d mesh.ny must be between 1 and 128",
                       "cfd3d.mesh.ny");
    }
    if (mesh.nz < 1 || mesh.nz > 256) {
        validation.add("NONPHYSICAL_INPUT", "cfd3d mesh.nz must be between 1 and 256",
                       "cfd3d.mesh.nz");
    }
    if (mesh.nx > 0 && mesh.ny > 0 && mesh.nz > 0 &&
        static_cast<std::size_t>(mesh.nx) * static_cast<std::size_t>(mesh.ny) *
                static_cast<std::size_t>(mesh.nz) >
            262144U) {
        validation.add("NONPHYSICAL_INPUT", "cfd3d mesh cell product must not exceed 262144",
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

    const auto make_sample = [&](double sample_time, double flow_m3_s) {
        double retained_total = 0.0;
        double capacity_total = 0.0;
        double weighted_temperature = 0.0;
        for (std::size_t node = 0; node < node_count; ++node) {
            retained_total += state.retained_water_kg[node];
            const double capacity = node_capacity(geometry, node % active_count,
                                                  state.temperature_k[node], bed.porosity, *water_);
            capacity_total += capacity;
            weighted_temperature += capacity * state.temperature_k[node];
        }
        ShotSample sample;
        sample.time_s = sample_time;
        sample.pressure_pa = recipe.pressure_pa.sample(sample_time);
        sample.inlet_temperature_k = recipe.inlet_temperature_k.sample(sample_time);
        sample.puck_temperature_k = capacity_total > kMassEpsilon
                                       ? weighted_temperature / capacity_total
                                       : coeff.initial_puck_temperature_k;
        sample.flow_m3_s = flow_m3_s;
        sample.beverage_mass_kg = beverage_mass_kg;
        sample.tds_fraction = beverage_mass_kg > kMassEpsilon
                                  ? solids_in_cup_kg / beverage_mass_kg
                                  : 0.0;
        sample.extraction_yield_fraction = recipe.dose_kg > kMassEpsilon
                                                ? solids_in_cup_kg / recipe.dose_kg
                                                : 0.0;
        sample.saturation = capacity_total > kMassEpsilon ? retained_total / capacity_total : 0.0;
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
        last_mobility = compute_mobility(geometry, state, material, bed.porosity,
                                         absolute_permeability_m2, *water_, coeff);
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
                face.water_rate_kg_s = face.total_flux_m3_s *
                                       water_->density_kg_m3(inlet_temperature_k);
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
            const double viscosity = water_->viscosity_pa_s(state.temperature_k[face.donor]);
            const double water_mobility = permeability * water_relative /
                                          viscosity;
            const double air_mobility = permeability * air_relative / kAirViscosityPaS;
            const double total_mobility = water_mobility + air_mobility;
            const double fraction_water = total_mobility > 0.0
                                              ? water_mobility / total_mobility
                                              : 0.0;
            const double donor_temperature = state.temperature_k[face.donor];
            const double density = water_->density_kg_m3(donor_temperature);
            const double rate = face.total_flux_m3_s * fraction_water * density;
            face.water_rate_kg_s = rate;
            water_rate_per_node[face.donor] += std::abs(rate);
            if (face.a != kInvalidNode) water_rate_per_node[face.a] += std::abs(rate);
            if (face.b != kInvalidNode) water_rate_per_node[face.b] += std::abs(rate);
        }
        const double max_time_step = std::min(config.dt_s, recipe.maximum_time_s - time_s);
        double dt = max_time_step;
        for (std::size_t node = 0; node < node_count; ++node) {
            const double capacity = node_capacity(geometry, node % active_count,
                                                  state.temperature_k[node], bed.porosity, *water_);
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
                face.energy_j = face.water_mass_kg *
                                water_->heat_capacity_j_kg_k(inlet_temperature_k) *
                                inlet_temperature_k;
            } else if (face.donor != kInvalidNode && face.water_mass_kg != 0.0) {
                const double leaving = water_rate_per_node[face.donor] * dt;
                const double scale = leaving > state.retained_water_kg[face.donor]
                                         ? state.retained_water_kg[face.donor] / leaving
                                         : 1.0;
                const double limited_scale = std::clamp(scale, 0.0, 1.0);
                face.water_mass_kg *= limited_scale;
                const double donor_concentration = concentration(state, face.donor);
                face.solids_mass_kg = face.water_mass_kg * donor_concentration;
                face.energy_j = face.water_mass_kg *
                                water_->heat_capacity_j_kg_k(state.temperature_k[face.donor]) *
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
                                  water_->density_kg_m3(state.temperature_k[face.a]);
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
            const double cp = water_->heat_capacity_j_kg_k(old_temperature);
            const double dose_share =
                recipe.dose_kg * geometry.aggregate_area_xy_m2[node % active_count] / basket_area_m2 /
                static_cast<double>(mesh.nz);
            const double loss_power = coeff.ambient_heat_loss_w_k *
                                      (geometry.aggregate_area_xy_m2[node % active_count] / basket_area_m2) *
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
                              0.0, state.retained_water_kg[node], 0.0},
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
            const double loss_power = coeff.ambient_heat_loss_w_k * (area / basket_area_m2) *
                                      (state.temperature_k[node] - coeff.ambient_temperature_k);
            ambient_energy_loss_j += loss_power * dt;
        }
        state = std::move(next);
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
                                         absolute_permeability_m2, *water_, coeff);
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
                           water_->heat_capacity_j_kg_k(state.temperature_k[node]) *
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
