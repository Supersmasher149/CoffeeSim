#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "espressolab/result.hpp"
#include "espressolab/types.hpp"
#include "espressolab/water_properties.hpp"

namespace espressolab {

// Fidelity level 4b: a Cartesian three-dimensional REV-scale mesh. The puck is
// circular in x/y and is divided into nz planes along the flow direction.
struct Cfd3dMesh {
    int nx = 32;
    int ny = 32;
    int nz = 16;
};

class Cfd3dField {
public:
    Cfd3dField() = default;
    Cfd3dField(int nx, int ny, int nz, double initial = 0.0);

    [[nodiscard]] int x_cells() const { return nx_; }
    [[nodiscard]] int y_cells() const { return ny_; }
    [[nodiscard]] int z_cells() const { return nz_; }
    [[nodiscard]] std::size_t size() const { return values_.size(); }

    [[nodiscard]] double& at(int x, int y, int z) { return values_[index(x, y, z)]; }
    [[nodiscard]] double at(int x, int y, int z) const { return values_[index(x, y, z)]; }
    [[nodiscard]] const std::vector<double>& values() const { return values_; }

private:
    [[nodiscard]] std::size_t index(int x, int y, int z) const {
        return static_cast<std::size_t>(x) +
               static_cast<std::size_t>(nx_) *
                   (static_cast<std::size_t>(y) + static_cast<std::size_t>(ny_) *
                                                     static_cast<std::size_t>(z));
    }

    int nx_ = 0;
    int ny_ = 0;
    int nz_ = 0;
    std::vector<double> values_;
};

// A dense, cell-centred material multiplier. An empty field means uniform
// material, which is the default. Values are permeability multipliers, not
// absolute permeabilities, and are constrained to [0.05, 20].
class Cfd3dMaterialField {
public:
    Cfd3dMaterialField() = default;
    Cfd3dMaterialField(int nx, int ny, int nz, double initial = 1.0);

    [[nodiscard]] bool empty() const { return values_.empty(); }
    [[nodiscard]] int x_cells() const { return nx_; }
    [[nodiscard]] int y_cells() const { return ny_; }
    [[nodiscard]] int z_cells() const { return nz_; }
    [[nodiscard]] std::size_t size() const { return values_.size(); }

    [[nodiscard]] double& at(int x, int y, int z) { return values_[index(x, y, z)]; }
    [[nodiscard]] double at(int x, int y, int z) const { return values_[index(x, y, z)]; }
    [[nodiscard]] const std::vector<double>& values() const { return values_; }

private:
    [[nodiscard]] std::size_t index(int x, int y, int z) const {
        return static_cast<std::size_t>(x) +
               static_cast<std::size_t>(nx_) *
                   (static_cast<std::size_t>(y) + static_cast<std::size_t>(ny_) *
                                                     static_cast<std::size_t>(z));
    }

    int nx_ = 0;
    int ny_ = 0;
    int nz_ = 0;
    std::vector<double> values_;
};

enum class Cfd3dCellClassification : std::uint8_t {
    outside,
    inside,
    cut,
    agglomerated,
};

struct Cfd3dSnapshot;
using Cfd3dSnapshotSink = std::function<void(const Cfd3dSnapshot&)>;

// Geometry is retained in the result so callers can verify the cut-cell
// construction without needing a second geometry implementation. Face arrays
// contain the open length of x-normal and y-normal faces, respectively.
struct Cfd3dGeometry {
    Cfd3dMesh mesh;
    double x_min_m = 0.0;
    double y_min_m = 0.0;
    double dx_m = 0.0;
    double dy_m = 0.0;
    double dz_m = 0.0;
    std::vector<double> cell_area_xy_m2;
    std::vector<double> effective_cell_area_xy_m2;
    std::vector<double> x_face_aperture_m;
    std::vector<double> y_face_aperture_m;
    std::vector<int> agglomerate_parent;
    std::vector<Cfd3dCellClassification> classification;
};

struct Cfd3dConfig {
    Cfd3dMesh mesh;
    double dt_s = 0.005;
    double sample_interval_s = 0.05;
    double cfl_number = 0.5;

    // PCG is intentionally bounded. A caller may tighten either setting but
    // cannot turn the fixed deterministic solve into an unbounded one.
    double pressure_tolerance = 1.0e-8;
    int pressure_max_iterations = 1000;

    // Full fields are emitted every second by default. Initial and final
    // snapshots are independently configurable and are emitted only when a
    // sink is set.
    double snapshot_interval_s = 1.0;
    bool snapshot_initial = true;
    bool snapshot_final = true;
    Cfd3dSnapshotSink snapshot_sink;

    Cfd3dMaterialField material;
};

struct Cfd3dDiagnostics {
    double max_total_velocity_divergence_1_s = 0.0;
    double pressure_residual = 0.0;
    long long pressure_iterations_total = 0;
    long long step_count = 0;
    double water_mass_residual_kg = 0.0;
    double solids_mass_residual_kg = 0.0;
    double energy_residual_j = 0.0;
    double max_courant_number = 0.0;
    int saturation_clamp_count = 0;
    int nonfinite_state_count = 0;
    int agglomerated_sliver_count = 0;
};

struct Cfd3dSnapshot {
    double time_s = 0.0;
    Cfd3dField pressure_pa;
    Cfd3dField saturation;
    Cfd3dField temperature_k;
    Cfd3dField pore_tds_fraction;
    Cfd3dField velocity_x_m_s;
    Cfd3dField velocity_y_m_s;
    Cfd3dField velocity_z_m_s;
};

struct Cfd3dResult {
    Cfd3dMesh mesh;
    std::string solver_version;
    TerminationReason termination = TerminationReason::not_terminated;
    double elapsed_time_s = 0.0;
    double beverage_mass_kg = 0.0;
    double tds_fraction = 0.0;
    double extraction_yield_fraction = 0.0;
    Cfd3dDiagnostics diagnostics;
    std::vector<SimulationWarning> warnings;
    std::vector<ShotSample> samples;
    Cfd3dGeometry geometry;

    Cfd3dField pressure_pa;
    Cfd3dField saturation;
    Cfd3dField temperature_k;
    Cfd3dField pore_tds_fraction;
    Cfd3dField permeability_multiplier;
    Cfd3dField velocity_x_m_s;
    Cfd3dField velocity_y_m_s;
    Cfd3dField velocity_z_m_s;
};

class Cfd3dSolver {
public:
    Cfd3dSolver();
    explicit Cfd3dSolver(std::shared_ptr<const WaterProperties> water);

    [[nodiscard]] Cfd3dResult run(const Recipe& recipe, const ModelCoefficients& coeff,
                                  const Cfd3dConfig& config = {}) const;

private:
    std::shared_ptr<const WaterProperties> water_;
};

}  // namespace espressolab
