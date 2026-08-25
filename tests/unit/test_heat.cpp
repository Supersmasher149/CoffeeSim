#include <catch_amalgamated.hpp>
#include <cmath>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/simulator.hpp"
#include "espressolab/units.hpp"

using namespace espressolab;

// Section 14.1: no temperature difference gives no transfer; the puck otherwise
// approaches the inlet temperature.
TEST_CASE("a puck already at inlet temperature does not drift", "[heat]") {
    Recipe recipe = testing::baseline_recipe();
    ModelCoefficients coeff = testing::baseline_coefficients();

    const double inlet_k = units::celsius_to_kelvin(93.0);
    recipe.inlet_temperature_k = PiecewiseLinearProfile::constant(inlet_k);
    coeff.initial_puck_temperature_k = inlet_k;
    coeff.ambient_temperature_k = inlet_k;  // isolate the water-side transfer
    coeff.ambient_heat_loss_w_k = 0.0;

    const ShotResult result = Simulator().run(recipe, coeff);
    for (const auto& sample : result.samples) {
        REQUIRE(sample.puck_temperature_k == Catch::Approx(inlet_k).margin(1.0e-9));
    }
}

TEST_CASE("a cold puck warms towards the inlet temperature", "[heat]") {
    Recipe recipe = testing::baseline_recipe();
    ModelCoefficients coeff = testing::baseline_coefficients();
    coeff.initial_puck_temperature_k = units::celsius_to_kelvin(35.0);

    const ShotResult result = Simulator().run(recipe, coeff);
    const double inlet_k = recipe.inlet_temperature_k.sample(0.0);

    REQUIRE(result.samples.front().puck_temperature_k < inlet_k);
    REQUIRE(result.samples.back().puck_temperature_k > result.samples.front().puck_temperature_k);
    REQUIRE(result.samples.back().puck_temperature_k <= inlet_k);

    // Monotone warm-up: with no ambient loss above inlet, temperature must
    // never overshoot the boundary it is chasing.
    for (const auto& sample : result.samples) {
        REQUIRE(sample.puck_temperature_k <= inlet_k + 1.0e-9);
        REQUIRE(std::isfinite(sample.puck_temperature_k));
    }
}

TEST_CASE("ambient loss pulls an unheated puck down, never below ambient", "[heat]") {
    Recipe recipe = testing::baseline_recipe();
    ModelCoefficients coeff = testing::baseline_coefficients();

    // No pressure means no water and no heat input: only ambient loss acts.
    recipe.pressure_pa = PiecewiseLinearProfile::constant(0.0);
    recipe.target_beverage_mass_kg.reset();
    coeff.initial_puck_temperature_k = units::celsius_to_kelvin(90.0);
    coeff.ambient_heat_loss_w_k = 2.0;

    const ShotResult result = Simulator().run(recipe, coeff);
    REQUIRE(result.samples.back().puck_temperature_k < result.samples.front().puck_temperature_k);
    REQUIRE(result.samples.back().puck_temperature_k > coeff.ambient_temperature_k);
}
