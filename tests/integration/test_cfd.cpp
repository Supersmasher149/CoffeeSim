#include <catch_amalgamated.hpp>
#include <array>
#include <cmath>
#include <limits>
#include <memory>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/cfd.hpp"
#include "espressolab/puck.hpp"
#include "espressolab/units.hpp"

using namespace espressolab;

namespace {

// An isothermal bed, where the exact solution is available: with no thermal
// gradient the viscosity is uniform, so the pressure field of a saturated
// uniform column is linear in depth and the flux obeys Darcy's law exactly.
ModelCoefficients isothermal_coefficients(double temperature_c) {
    ModelCoefficients coeff = testing::baseline_coefficients();
    coeff.initial_puck_temperature_k = units::celsius_to_kelvin(temperature_c);
    coeff.ambient_temperature_k = units::celsius_to_kelvin(temperature_c);
    coeff.ambient_heat_loss_w_k = 0.0;
    return coeff;
}

Recipe isothermal_recipe(double temperature_c) {
    Recipe recipe = testing::baseline_recipe();
    recipe.pressure_pa = PiecewiseLinearProfile::constant(units::bar_to_pa(9.0));
    recipe.inlet_temperature_k =
        PiecewiseLinearProfile::constant(units::celsius_to_kelvin(temperature_c));
    return recipe;
}

CfdConfig mesh(int radial, int axial) {
    CfdConfig config;
    config.mesh.radial_cells = radial;
    config.mesh.axial_cells = axial;
    return config;
}

class TestWaterProperties final : public WaterProperties {
public:
    TestWaterProperties(double density, double viscosity, double heat_capacity)
        : density_(density), viscosity_(viscosity), heat_capacity_(heat_capacity) {}

    [[nodiscard]] double density_kg_m3(double) const override { return density_; }
    [[nodiscard]] double viscosity_pa_s(double) const override { return viscosity_; }
    [[nodiscard]] double heat_capacity_j_kg_k(double) const override { return heat_capacity_; }
    [[nodiscard]] double min_temperature_k() const override { return 273.15; }
    [[nodiscard]] double max_temperature_k() const override { return 373.15; }

private:
    double density_;
    double viscosity_;
    double heat_capacity_;
};

}  // namespace

// Verification, not validation: these check the solver against the equations it
// claims to solve. Whether those equations describe real espresso is a
// calibration question this suite cannot answer.

TEST_CASE("the total velocity field is discretely divergence free", "[cfd][verification]") {
    // div(u_t) = 0 is the pressure equation. If the discrete divergence is not
    // ~0 the elliptic solve is not converged and nothing downstream is trustworthy.
    const CfdResult result =
        CfdSolver().run(testing::baseline_recipe(), testing::baseline_coefficients(), mesh(8, 16));

    INFO("max |div u_t| = " << result.diagnostics.max_total_velocity_divergence_1_s);
    REQUIRE(result.diagnostics.max_total_velocity_divergence_1_s < 1.0e-5);
    REQUIRE(result.diagnostics.pressure_residual < 1.0e-8);
}

TEST_CASE("a uniform bed produces an axisymmetric solution", "[cfd][verification]") {
    // With no radial variation in the inputs, every field must be independent
    // of r. A radial gradient here would mean the mesh metrics or the transport
    // are introducing structure that the physics does not contain.
    const CfdResult result =
        CfdSolver().run(testing::baseline_recipe(), testing::baseline_coefficients(), mesh(6, 10));

    // The red-black sweep visits cells in a checkerboard order, so the
    // converged solution carries ordering noise at roughly the pressure
    // tolerance. Anything larger would be real radial structure.
    for (int j = 0; j < result.saturation.axial_cells(); ++j) {
        for (int i = 1; i < result.saturation.radial_cells(); ++i) {
            INFO("row " << j << " column " << i);
            REQUIRE(result.pressure_pa.at(i, j) ==
                    Catch::Approx(result.pressure_pa.at(0, j)).epsilon(1.0e-7));
            REQUIRE(result.saturation.at(i, j) ==
                    Catch::Approx(result.saturation.at(0, j)).epsilon(1.0e-7));
            REQUIRE(result.temperature_k.at(i, j) ==
                    Catch::Approx(result.temperature_k.at(0, j)).epsilon(1.0e-7));
            // No radial flow can exist in an axisymmetric uniform column.
            REQUIRE(std::abs(result.radial_velocity_m_s.at(i, j)) < 1.0e-9);
        }
    }
}

TEST_CASE("an isothermal saturated column matches the exact linear solution",
          "[cfd][verification]") {
    // Method of exact solutions. Uniform mobility makes div(lambda grad p) = 0
    // reduce to d2p/dz2 = 0, whose solution is linear in depth: every cell must
    // therefore drop the same pressure.
    const double temperature_c = 93.0;
    const CfdResult result = CfdSolver().run(isothermal_recipe(temperature_c),
                                             isothermal_coefficients(temperature_c), mesh(4, 16));

    const int nz = result.pressure_pa.axial_cells();
    std::vector<double> drops;
    for (int j = 0; j + 1 < nz; ++j) {
        drops.push_back(result.pressure_pa.at(0, j) - result.pressure_pa.at(0, j + 1));
    }
    REQUIRE(drops.size() > 4);

    // The exact solution assumes uniform mobility. The bed is isothermal, so
    // viscosity is uniform, but saturation still varies down the column and
    // relative permeability carries that into the mobility. The measured
    // departure is about 5e-4 and shrinks under refinement (5.9e-4, 5.5e-4 and
    // 4.3e-4 at 8, 16 and 32 axial cells), which is what identifies it as the
    // saturation profile and the discretization rather than a solver defect.
    const double first = drops.front();
    for (std::size_t i = 0; i < drops.size(); ++i) {
        INFO("cell " << i << " drop " << drops[i] << " vs " << first);
        REQUIRE(drops[i] == Catch::Approx(first).epsilon(1.0e-3));
    }

    // The column drains downward everywhere and nowhere upward.
    for (int j = 0; j + 1 < nz; ++j) {
        REQUIRE(result.pressure_pa.at(0, j) > result.pressure_pa.at(0, j + 1));
        REQUIRE(result.axial_velocity_m_s.at(0, j) > 0.0);
    }
}

TEST_CASE("the Darcy velocity matches the analytic value", "[cfd][verification]") {
    // With a uniform isothermal saturated bed the superficial velocity is
    // u = (k / mu) * dp/dz, evaluated from the solver's own coefficients.
    const double temperature_c = 93.0;
    const ModelCoefficients coeff = isothermal_coefficients(temperature_c);
    const Recipe recipe = isothermal_recipe(temperature_c);
    const CfdResult result = CfdSolver().run(recipe, coeff, mesh(4, 16));

    const PuckGeometry bed =
        compress_puck(recipe, coeff, units::bar_to_pa(9.0) - coeff.outlet_pressure_pa);
    const double permeability =
        kozeny_carman_permeability(recipe.particle_diameter_m, bed.porosity, coeff.kozeny_constant) *
        distribution_factor(recipe.particle_spread_factor, coeff.distribution_factor_floor);
    const double viscosity = TabulatedWaterProperties().viscosity_pa_s(
        units::celsius_to_kelvin(temperature_c));

    const int nz = result.pressure_pa.axial_cells();
    const double dz = bed.depth_m / static_cast<double>(nz);
    const int j = nz / 2;
    const double gradient = (result.pressure_pa.at(0, j - 1) - result.pressure_pa.at(0, j + 1)) /
                            (2.0 * dz);
    const double analytic = permeability / viscosity * gradient;

    INFO("analytic " << analytic << " solver " << result.axial_velocity_m_s.at(0, j));
    // The air phase carries a small share of the total mobility even at full
    // saturation, so the agreement is close rather than exact.
    REQUIRE(result.axial_velocity_m_s.at(0, j) == Catch::Approx(analytic).epsilon(0.01));
}

TEST_CASE("mass balances close on the CFD mesh", "[cfd][verification]") {
    for (const auto& size : {std::pair{3, 6}, std::pair{6, 12}, std::pair{10, 20}}) {
        INFO("mesh " << size.first << " x " << size.second);
        const CfdResult result = CfdSolver().run(testing::baseline_recipe(),
                                                 testing::baseline_coefficients(),
                                                 mesh(size.first, size.second));
        REQUIRE(std::abs(result.diagnostics.water_mass_residual_kg) < 1.0e-9);
        REQUIRE(std::abs(result.diagnostics.solids_mass_residual_kg) < 1.0e-9);

        // Saturation is a volume fraction and cannot leave [0, 1].
        for (int j = 0; j < result.saturation.axial_cells(); ++j) {
            for (int i = 0; i < result.saturation.radial_cells(); ++i) {
                REQUIRE(result.saturation.at(i, j) >= 0.0);
                REQUIRE(result.saturation.at(i, j) <= 1.0 + 1.0e-9);
            }
        }
        REQUIRE(result.diagnostics.saturation_clamp_count == 0);
    }
}

TEST_CASE("the CFD solution converges under mesh refinement", "[cfd][verification]") {
    // A discretization that does not settle as the mesh is refined is not
    // solving anything. Successive refinements must move the answer less.
    const CfdResult a = CfdSolver().run(testing::baseline_recipe(),
                                        testing::baseline_coefficients(), mesh(2, 4));
    const CfdResult b = CfdSolver().run(testing::baseline_recipe(),
                                        testing::baseline_coefficients(), mesh(4, 8));
    const CfdResult c = CfdSolver().run(testing::baseline_recipe(),
                                        testing::baseline_coefficients(), mesh(8, 16));
    const CfdResult d = CfdSolver().run(testing::baseline_recipe(),
                                        testing::baseline_coefficients(), mesh(16, 32));

    const auto gap = [](const CfdResult& x, const CfdResult& y) {
        return std::abs(x.extraction_yield_fraction - y.extraction_yield_fraction);
    };
    INFO("gaps " << gap(b, a) << " " << gap(c, b) << " " << gap(d, c));
    REQUIRE(gap(b, a) > gap(c, b));
    REQUIRE(gap(c, b) > gap(d, c));
}

TEST_CASE("the CFD solver is deterministic", "[cfd][verification]") {
    const CfdConfig config = mesh(5, 9);
    const CfdResult first =
        CfdSolver().run(testing::baseline_recipe(), testing::baseline_coefficients(), config);
    const CfdResult second =
        CfdSolver().run(testing::baseline_recipe(), testing::baseline_coefficients(), config);

    REQUIRE(first.elapsed_time_s == second.elapsed_time_s);
    REQUIRE(first.beverage_mass_kg == second.beverage_mass_kg);
    REQUIRE(first.pressure_pa.values() == second.pressure_pa.values());
    REQUIRE(first.saturation.values() == second.saturation.values());
    REQUIRE(first.temperature_k.values() == second.temperature_k.values());
}

TEST_CASE("a channelled recipe resolves a radial flow structure", "[cfd][integration]") {
    // The two-dimensional mesh is what makes this different from the parallel
    // regions of Level 2: the channel is a place in the puck, and the solver
    // resolves faster flow there rather than being told the split.
    const CfdResult result = CfdSolver().run(testing::channelled_recipe(),
                                             testing::baseline_coefficients(), mesh(10, 12));

    const int nz = result.axial_velocity_m_s.axial_cells();
    const int nr = result.axial_velocity_m_s.radial_cells();
    const int mid = nz / 2;
    const double axis_velocity = result.axial_velocity_m_s.at(0, mid);
    const double wall_velocity = result.axial_velocity_m_s.at(nr - 1, mid);

    INFO("axis " << axis_velocity << " wall " << wall_velocity);
    // channelled.json puts the high permeability region in the outer ring.
    REQUIRE(wall_velocity > axis_velocity);
    // A radial pressure gradient exists, which a one-dimensional model cannot
    // represent at all.
    bool radial_flow_present = false;
    for (int j = 0; j < nz && !radial_flow_present; ++j) {
        for (int i = 0; i < nr; ++i) {
            if (std::abs(result.radial_velocity_m_s.at(i, j)) > 1.0e-12) {
                radial_flow_present = true;
                break;
            }
        }
    }
    REQUIRE(radial_flow_present);
}

TEST_CASE("CFD mesh sizes outside the supported range are rejected", "[cfd][artifacts]") {
    const Recipe recipe = testing::baseline_recipe();
    const ModelCoefficients coeff = testing::baseline_coefficients();

    REQUIRE_THROWS_AS(CfdSolver().run(recipe, coeff, mesh(0, 8)), InvalidInputError);
    REQUIRE_THROWS_AS(CfdSolver().run(recipe, coeff, mesh(8, 0)), InvalidInputError);
    REQUIRE_THROWS_AS(CfdSolver().run(recipe, coeff, mesh(129, 8)), InvalidInputError);
    REQUIRE_THROWS_AS(CfdSolver().run(recipe, coeff, mesh(8, 257)), InvalidInputError);

    CfdConfig bad_step = mesh(4, 8);
    bad_step.dt_s = 0.0;
    REQUIRE_THROWS_AS(CfdSolver().run(recipe, coeff, bad_step), InvalidInputError);

    CfdConfig bad_pressure = mesh(4, 8);
    bad_pressure.pressure_max_iterations = 0;
    REQUIRE_THROWS_AS(CfdSolver().run(recipe, coeff, bad_pressure), InvalidInputError);
}

TEST_CASE("CFD rejects null and nonphysical water-properties providers", "[cfd][artifacts]") {
    const std::shared_ptr<const WaterProperties> null_provider;
    REQUIRE_THROWS_AS(CfdSolver(null_provider), InvalidInputError);

    const std::array<double, 4> invalid_values{
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        0.0,
        -1.0,
    };
    const Recipe recipe = testing::baseline_recipe();
    const ModelCoefficients coeff = testing::baseline_coefficients();

    for (const double invalid : invalid_values) {
        INFO("invalid density " << invalid);
        REQUIRE_THROWS_AS(
            CfdSolver(std::make_shared<TestWaterProperties>(invalid, 1.0e-3, 4184.0))
                .run(recipe, coeff, mesh(2, 2)),
            InvalidInputError);
        INFO("invalid viscosity " << invalid);
        REQUIRE_THROWS_AS(
            CfdSolver(std::make_shared<TestWaterProperties>(998.0, invalid, 4184.0))
                .run(recipe, coeff, mesh(2, 2)),
            InvalidInputError);
        INFO("invalid heat capacity " << invalid);
        REQUIRE_THROWS_AS(
            CfdSolver(std::make_shared<TestWaterProperties>(998.0, 1.0e-3, invalid))
                .run(recipe, coeff, mesh(2, 2)),
            InvalidInputError);
    }
}

TEST_CASE("CFD reports pressure non-convergence", "[cfd][verification]") {
    CfdConfig config = mesh(8, 16);
    config.pressure_max_iterations = 1;

    const CfdResult result =
        CfdSolver().run(testing::baseline_recipe(), testing::baseline_coefficients(), config);

    REQUIRE(result.termination == TerminationReason::numerical_failure);
    REQUIRE_FALSE(result.warnings.empty());
    REQUIRE(result.warnings.front().code == "NUMERICAL_FAILURE");
}

TEST_CASE("CFD rejects saturation overshoot in strict mode", "[cfd][verification]") {
    CfdConfig config = mesh(4, 8);
    config.dt_s = 1.0;
    config.sample_interval_s = 1.0;

    const CfdResult result =
        CfdSolver().run(testing::baseline_recipe(), testing::baseline_coefficients(), config);

    REQUIRE(result.termination == TerminationReason::invalid_state);
    REQUIRE(result.diagnostics.saturation_clamp_count > 0);
    REQUIRE_FALSE(result.warnings.empty());
    REQUIRE(result.warnings.back().code == "SATURATION_INVARIANT");
}
