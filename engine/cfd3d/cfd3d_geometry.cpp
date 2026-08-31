#include "cfd3d_geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

#include "espressolab/validation.hpp"

namespace espressolab {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kGeometryRelativeTolerance = 1.0e-12;
constexpr double kSliverFraction = 0.05;

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

}  // namespace

std::size_t field_size(int nx, int ny, int nz) {
    if (nx < 1 || ny < 1 || nz < 1) {
        throw std::invalid_argument("Cfd3dField dimensions must be positive");
    }
    if (nx > kMaximumMeshNx || ny > kMaximumMeshNy || nz > kMaximumMeshNz) {
        throw std::invalid_argument("Cfd3dField dimensions exceed the maximum supported mesh size");
    }
    const auto ux = static_cast<std::uint64_t>(nx);
    const auto uy = static_cast<std::uint64_t>(ny);
    const auto uz = static_cast<std::uint64_t>(nz);
    if (ux > std::numeric_limits<std::uint64_t>::max() / uy ||
        ux * uy > std::numeric_limits<std::uint64_t>::max() / uz) {
        throw std::invalid_argument("Cfd3dField dimensions overflow the field size");
    }
    const std::uint64_t count = ux * uy * uz;
    if (count > kMaximumMeshCells) {
        throw std::invalid_argument("Cfd3dField cell count exceeds the maximum supported mesh size");
    }
    return static_cast<std::size_t>(count);
}

std::size_t xy_index(int x, int y, int nx) {
    return static_cast<std::size_t>(x) + static_cast<std::size_t>(nx) * static_cast<std::size_t>(y);
}

std::size_t node_index(std::size_t active_xy, int z, std::size_t active_count) {
    return active_xy + active_count * static_cast<std::size_t>(z);
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

}  // namespace espressolab
