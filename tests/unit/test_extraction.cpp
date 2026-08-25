#include <catch_amalgamated.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/extraction.hpp"
#include "espressolab/units.hpp"

using namespace espressolab;

// Section 14.1: no saturation gives zero extraction; available mass never goes
// negative (the latter is covered by the invariant tests).
TEST_CASE("a dry puck extracts nothing", "[extraction]") {
    REQUIRE(saturation_factor(0.0) == 0.0);

    const Recipe recipe = testing::baseline_recipe();
    const ModelCoefficients coeff = testing::baseline_coefficients();
    ShotState state;
    state.liquid_saturation = 0.0;
    state.puck_temperature_k = units::celsius_to_kelvin(93.0);

    REQUIRE(extraction_rate_coefficient(state, recipe, coeff, 2.0e-6) == 0.0);
}

TEST_CASE("extraction speeds up with temperature", "[extraction]") {
    const ModelCoefficients coeff = testing::baseline_coefficients();
    const double cool = temperature_factor(units::celsius_to_kelvin(85.0), coeff);
    const double warm = temperature_factor(units::celsius_to_kelvin(96.0), coeff);

    REQUIRE(warm > cool);
    REQUIRE(temperature_factor(coeff.reference_temperature_k, coeff) == Catch::Approx(1.0));
}

TEST_CASE("the temperature factor is clamped against absurd input", "[extraction]") {
    const ModelCoefficients coeff = testing::baseline_coefficients();
    REQUIRE(temperature_factor(1.0, coeff) > 0.0);            // no underflow to zero
    REQUIRE(temperature_factor(1.0e6, coeff) < 1.0e5);        // no runaway
    REQUIRE(temperature_factor(-5.0, coeff) == 0.0);          // nonphysical
}

TEST_CASE("finer grinds extract faster and the factor stays bounded", "[extraction]") {
    const ModelCoefficients coeff = testing::baseline_coefficients();
    REQUIRE(grind_factor(units::microns_to_m(250.0), coeff) >
            grind_factor(units::microns_to_m(450.0), coeff));
    REQUIRE(grind_factor(coeff.reference_particle_diameter_m, coeff) == Catch::Approx(1.0));
    REQUIRE(grind_factor(1.0e-12, coeff) <= 20.0);
    REQUIRE(grind_factor(0.0, coeff) == 0.0);
}

TEST_CASE("the flow contact factor never exceeds one", "[extraction]") {
    for (double q = 0.0; q < 1.0e-3; q += 1.0e-5) {
        const double factor = flow_contact_factor(q, 1.5e-6);
        REQUIRE(factor >= 0.0);
        REQUIRE(factor < 1.0);
    }
    REQUIRE(flow_contact_factor(0.0, 1.5e-6) == 0.0);
    REQUIRE(flow_contact_factor(1.5e-6, 1.5e-6) == Catch::Approx(0.5));
}
