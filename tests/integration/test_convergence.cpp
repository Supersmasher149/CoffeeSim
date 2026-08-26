#include <catch_amalgamated.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/simulator.hpp"
#include "espressolab/units.hpp"

using namespace espressolab;

// Section 9.4: run the same baseline shot at 0.02, 0.01 and 0.005 s. Final
// beverage mass, TDS and extraction yield must converge; if halving the step
// still moves the answer, the default step is wrong.
TEST_CASE("results converge as the time step halves", "[convergence]") {
    // Stopping on beverage mass lands on a different step for each dt, which
    // adds a discretisation jitter of its own to the final metrics. Convergence
    // of the integration is measured against a fixed stop time; the mass-target
    // case is checked for agreement, not for monotone convergence, below.
    Recipe recipe = testing::baseline_recipe();
    recipe.target_beverage_mass_kg.reset();
    recipe.maximum_time_s = 30.0;
    const ModelCoefficients coeff = testing::baseline_coefficients();

    const auto run_at = [&](double dt) {
        SimulationConfig config;
        config.dt_s = dt;
        config.sample_interval_s = 0.05;
        return Simulator().run(recipe, coeff, config);
    };

    const ShotResult coarse = run_at(0.02);
    const ShotResult medium = run_at(0.01);
    const ShotResult fine = run_at(0.005);

    const double coarse_to_medium =
        std::abs(coarse.summary.extraction_yield_fraction - medium.summary.extraction_yield_fraction);
    const double medium_to_fine =
        std::abs(medium.summary.extraction_yield_fraction - fine.summary.extraction_yield_fraction);

    // Halving the step must shrink the change, and the remaining difference at
    // the shipping step size must be small enough to ignore.
    REQUIRE(medium_to_fine < coarse_to_medium);
    REQUIRE(medium_to_fine < 0.005);  // under half a percentage point of yield

    REQUIRE(units::kg_to_grams(medium.summary.beverage_mass_kg) ==
            Catch::Approx(units::kg_to_grams(fine.summary.beverage_mass_kg)).margin(0.2));
    REQUIRE(medium.summary.tds_fraction == Catch::Approx(fine.summary.tds_fraction).margin(0.003));
    REQUIRE(medium.summary.elapsed_time_s ==
            Catch::Approx(fine.summary.elapsed_time_s).margin(0.5));
}

TEST_CASE("a mass-target shot agrees across step sizes", "[convergence]") {
    const Recipe recipe = testing::baseline_recipe();
    const ModelCoefficients coeff = testing::baseline_coefficients();

    const auto run_at = [&](double dt) {
        SimulationConfig config;
        config.dt_s = dt;
        return Simulator().run(recipe, coeff, config);
    };

    const ShotResult medium = run_at(0.01);
    const ShotResult fine = run_at(0.005);

    // The stop condition is discrete, so these agree within a tolerance rather
    // than converging monotonically.
    REQUIRE(units::kg_to_grams(medium.summary.beverage_mass_kg) ==
            Catch::Approx(units::kg_to_grams(fine.summary.beverage_mass_kg)).margin(0.2));
    REQUIRE(medium.summary.extraction_yield_fraction ==
            Catch::Approx(fine.summary.extraction_yield_fraction).margin(0.005));
    REQUIRE(medium.summary.elapsed_time_s ==
            Catch::Approx(fine.summary.elapsed_time_s).margin(0.5));
}

TEST_CASE("parallel-region results converge as the time step halves", "[convergence][regions]") {
    Recipe recipe = testing::channelled_recipe();
    recipe.target_beverage_mass_kg.reset();
    recipe.maximum_time_s = 30.0;
    const ModelCoefficients coeff = testing::baseline_coefficients();

    const auto run_at = [&](double dt) {
        SimulationConfig config;
        config.dt_s = dt;
        return Simulator().run(recipe, coeff, config);
    };

    const ShotResult medium = run_at(0.01);
    const ShotResult fine = run_at(0.005);
    REQUIRE(medium.regions.size() == 2);
    REQUIRE(fine.regions.size() == 2);
    REQUIRE(medium.summary.beverage_mass_kg ==
            Catch::Approx(fine.summary.beverage_mass_kg).margin(2.0e-4));
    REQUIRE(medium.summary.extraction_yield_fraction ==
            Catch::Approx(fine.summary.extraction_yield_fraction).margin(0.003));
    REQUIRE(medium.regions[1].flow_fraction ==
            Catch::Approx(fine.regions[1].flow_fraction).margin(0.002));
}

TEST_CASE("the sample interval does not change the physics", "[convergence]") {
    const Recipe recipe = testing::baseline_recipe();
    const ModelCoefficients coeff = testing::baseline_coefficients();

    SimulationConfig dense;
    dense.sample_interval_s = 0.01;
    SimulationConfig sparse;
    sparse.sample_interval_s = 0.25;

    const ShotResult a = Simulator().run(recipe, coeff, dense);
    const ShotResult b = Simulator().run(recipe, coeff, sparse);

    REQUIRE(a.samples.size() > b.samples.size());
    REQUIRE(a.summary.elapsed_time_s == Catch::Approx(b.summary.elapsed_time_s));
    REQUIRE(a.summary.beverage_mass_kg == Catch::Approx(b.summary.beverage_mass_kg));
    REQUIRE(a.summary.extraction_yield_fraction ==
            Catch::Approx(b.summary.extraction_yield_fraction));
    REQUIRE(a.summary.average_flow_m3_s == Catch::Approx(b.summary.average_flow_m3_s));
}

TEST_CASE("samples land on the requested interval rather than solver steps", "[convergence]") {
    Recipe recipe = testing::baseline_recipe();
    recipe.target_beverage_mass_kg.reset();
    recipe.maximum_time_s = 10.0;
    SimulationConfig config;
    config.dt_s = 0.1;
    config.sample_interval_s = 0.15;

    const ShotResult result = Simulator().run(recipe, testing::baseline_coefficients(), config);
    REQUIRE(result.samples.front().time_s == Catch::Approx(0.0));
    REQUIRE(result.samples[1].time_s == Catch::Approx(0.15));
    REQUIRE(result.samples[2].time_s == Catch::Approx(0.30));
    REQUIRE(result.samples.back().time_s == Catch::Approx(result.summary.elapsed_time_s));
}
