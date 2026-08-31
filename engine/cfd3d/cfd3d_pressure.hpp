#pragma once

// Internal (non-public) header shared only between the cfd3d translation
// units: transmissibility assembly plus the geometric-multigrid/PCG pressure
// solve. PressureLevel is solver-internal and must not leak into
// include/espressolab/cfd3d.hpp.

#include <array>
#include <limits>
#include <utility>
#include <vector>

#include "cfd3d_geometry.hpp"
#include "espressolab/execution.hpp"

namespace espressolab {

double harmonic_mean(double first, double second);

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

PressureLevel build_fine_pressure_level(const GeometryInternal& geometry,
                                        const std::vector<double>& mobility,
                                        double inlet_pressure_pa, double outlet_pressure_pa);

struct PressureSolveResult {
    bool converged = false;
    int iterations = 0;
    double residual = std::numeric_limits<double>::infinity();
};

PressureSolveResult solve_pressure(const PressureLevel& system, std::vector<double>& pressure,
                                   double tolerance, int maximum_iterations,
                                   const CancellationCallback& is_cancelled);

}  // namespace espressolab
