#pragma once
#include <memory>
#include <string>
#include <vector>

#include "espressolab/execution.hpp"
#include "espressolab/result.hpp"
#include "espressolab/types.hpp"
#include "espressolab/water_properties.hpp"

namespace espressolab {

// Fidelity level 4: a two-dimensional axisymmetric finite-volume solver for
// two-phase flow through the puck.
//
// What this solves, stated exactly so nothing is overclaimed:
//
//   total velocity      u_t = -lambda_t grad(p),  lambda_t = lambda_w + lambda_a
//   pressure            div( lambda_t grad(p) ) = 0        (elliptic, solved on the mesh)
//   water saturation    phi dS/dt + div( f_w u_t ) = 0     (hyperbolic, upwinded)
//   enthalpy, solute    advected on the water flux, with the local source terms
//
// The momentum closure is Darcy-Forchheimer at the representative-elementary-
// volume scale, which is the standard porous-media momentum equation: inertia
// inside the pores is not resolved because the pore geometry is not resolved.
// This is REV-scale CFD, not pore-resolved direct numerical simulation.
struct CfdMesh {
    int radial_cells = 12;
    int axial_cells = 24;
};

struct CfdConfig {
    CfdMesh mesh;
    double dt_s = 0.005;
    double sample_interval_s = 0.05;
    // Pressure solve: successive over-relaxation to a fixed relative residual.
    // A fixed sweep order keeps the result reproducible.
    double pressure_tolerance = 1.0e-10;
    int pressure_max_iterations = 20000;
    double relaxation = 1.7;
    // Forchheimer inertial correction. Zero recovers pure Darcy momentum.
    double forchheimer_beta_1_m = 0.0;
    bool strict_invariants = true;
};

// A cell-centred scalar field over the (r, z) mesh, r fastest.
class CfdField {
public:
    CfdField() = default;
    CfdField(int radial_cells, int axial_cells, double initial = 0.0)
        : nr_(radial_cells), nz_(axial_cells),
          values_(static_cast<std::size_t>(radial_cells) * static_cast<std::size_t>(axial_cells),
                  initial) {}

    [[nodiscard]] int radial_cells() const { return nr_; }
    [[nodiscard]] int axial_cells() const { return nz_; }
    [[nodiscard]] double& at(int i, int j) { return values_[index(i, j)]; }
    [[nodiscard]] double at(int i, int j) const { return values_[index(i, j)]; }
    [[nodiscard]] const std::vector<double>& values() const { return values_; }

private:
    [[nodiscard]] std::size_t index(int i, int j) const {
        return static_cast<std::size_t>(j) * static_cast<std::size_t>(nr_) +
               static_cast<std::size_t>(i);
    }
    int nr_ = 0;
    int nz_ = 0;
    std::vector<double> values_;
};

// Verification quantities. These are how the solver is checked against the
// equations it claims to solve, rather than against a plausible-looking curve.
struct CfdDiagnostics {
    double max_total_velocity_divergence_1_s = 0.0;  // discrete div(u_t), should be ~0
    double pressure_residual = 0.0;                  // final relative residual
    long long pressure_iterations_total = 0;
    long long step_count = 0;
    double water_mass_residual_kg = 0.0;
    double solids_mass_residual_kg = 0.0;
    int saturation_clamp_count = 0;
    double max_courant_number = 0.0;
};

struct CfdResult {
    CfdMesh mesh;
    std::string solver_version;
    TerminationReason termination = TerminationReason::not_terminated;
    double elapsed_time_s = 0.0;
    double beverage_mass_kg = 0.0;
    double tds_fraction = 0.0;
    double extraction_yield_fraction = 0.0;
    CfdDiagnostics diagnostics;
    std::vector<SimulationWarning> warnings;
    std::vector<ShotSample> samples;
    // Final fields, for inspection and for the verification tests.
    CfdField pressure_pa;
    CfdField saturation;
    CfdField temperature_k;
    CfdField pore_tds_fraction;
    CfdField axial_velocity_m_s;
    CfdField radial_velocity_m_s;
};

class CfdSolver {
public:
    CfdSolver();
    explicit CfdSolver(std::shared_ptr<const WaterProperties> water);

    [[nodiscard]] CfdResult run(const Recipe& recipe, const ModelCoefficients& coeff,
                                const CfdConfig& config = {},
                                const CancellationCallback& is_cancelled = {}) const;

private:
    std::shared_ptr<const WaterProperties> water_;
};

}  // namespace espressolab
