#pragma once

// Internal (non-public) header shared only between the cfd3d translation
// units. GeometryInternal wraps the public Cfd3dGeometry with cut-cell
// bookkeeping (root_for_xy, active_roots, ...) that pressure assembly and
// solver orchestration both need but that must not leak into
// include/espressolab/cfd3d.hpp -- callers only ever see Cfd3dGeometry.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "espressolab/cfd3d.hpp"

namespace espressolab {

// Audit P3, issue #20: field_size() multiplied raw dimensions with no
// checked multiplication and no maximum, and Cfd3dField/Cfd3dMaterialField's
// 4-arg constructors are public -- a caller constructing one directly
// (bypassing both Cfd3dSolver::run()'s own mesh check, lines below, and
// cfd3d_artifact_io::validate_mesh_bounds, issue #5's equivalent guard for
// the JSON loader) could overflow std::size_t with large-but-positive
// dimensions, silently truncating the allocation while nx_/ny_/nz_ still
// hold the original (too-large) values used for indexing in at() -- an
// out-of-bounds vector access, not just an allocation-size mistake.
// kMaximumMesh{Nx,Ny,Nz,Cells} mirror the exact limits Cfd3dSolver::run()
// enforces just below and cfd3d_artifact_io::validate_mesh_bounds enforces
// in the loader; all three must move together.
constexpr int kMaximumMeshNx = 128;
constexpr int kMaximumMeshNy = 128;
constexpr int kMaximumMeshNz = 256;
constexpr std::uint64_t kMaximumMeshCells = 262144;

std::size_t field_size(int nx, int ny, int nz);

struct GeometryInternal {
    Cfd3dGeometry public_geometry;
    std::vector<int> root_for_xy;
    std::vector<int> active_roots;
    std::vector<int> active_index_for_root;
    std::vector<double> aggregate_area_xy_m2;
};

std::size_t xy_index(int x, int y, int nx);

std::size_t node_index(std::size_t active_xy, int z, std::size_t active_count);

GeometryInternal build_geometry(double radius_m, double depth_m, const Cfd3dMesh& mesh);

bool material_is_uniform_in_xy(const GeometryInternal& geometry, const Cfd3dMaterialField& material);

void collapse_uniform_xy_state(GeometryInternal& geometry);

}  // namespace espressolab
