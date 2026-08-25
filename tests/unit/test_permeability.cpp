#include <catch_amalgamated.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/puck.hpp"
#include "espressolab/units.hpp"

using namespace espressolab;

// Section 14.1: monotonic particle-size response and bounded compression.
TEST_CASE("permeability rises with the square of particle diameter", "[permeability]") {
    const double small = kozeny_carman_permeability(units::microns_to_m(200.0), 0.42, 180.0);
    const double large = kozeny_carman_permeability(units::microns_to_m(400.0), 0.42, 180.0);
    REQUIRE(small > 0.0);
    REQUIRE(large > small);
    REQUIRE(large / small == Catch::Approx(4.0));  // d^2 dependence
}

TEST_CASE("permeability rises with porosity and is monotonic in grind", "[permeability]") {
    REQUIRE(kozeny_carman_permeability(350.0e-6, 0.45, 180.0) >
            kozeny_carman_permeability(350.0e-6, 0.35, 180.0));

    double previous = 0.0;
    for (double um = 150.0; um <= 800.0; um += 25.0) {
        const double k = kozeny_carman_permeability(units::microns_to_m(um), 0.42, 180.0);
        REQUIRE(k > previous);
        previous = k;
    }
}

TEST_CASE("nonphysical permeability inputs return zero rather than dividing", "[permeability]") {
    REQUIRE(kozeny_carman_permeability(0.0, 0.42, 180.0) == 0.0);
    REQUIRE(kozeny_carman_permeability(-1.0e-4, 0.42, 180.0) == 0.0);
    REQUIRE(kozeny_carman_permeability(350.0e-6, 0.42, 0.0) == 0.0);
}

TEST_CASE("the distribution factor is bounded and penalises broad grinds",
          "[permeability]") {
    REQUIRE(distribution_factor(0.1, 0.05) > distribution_factor(0.9, 0.05));
    for (double spread = 0.0; spread <= 1.0; spread += 0.05) {
        const double factor = distribution_factor(spread, 0.05);
        REQUIRE(factor >= 0.05);
        REQUIRE(factor <= 1.0);
    }
}

TEST_CASE("the wetting factor ramps from the dry multiplier to one", "[permeability]") {
    REQUIRE(wetting_factor(0.0, 0.25) == Catch::Approx(0.25));
    REQUIRE(wetting_factor(1.0, 0.25) == Catch::Approx(1.0));
    REQUIRE(wetting_factor(0.5, 0.25) > 0.25);
    REQUIRE(wetting_factor(0.5, 0.25) < 1.0);
    REQUIRE(wetting_factor(5.0, 0.25) == Catch::Approx(1.0));  // clamped
}

TEST_CASE("puck compression stays inside its configured bound", "[permeability]") {
    const Recipe recipe = testing::baseline_recipe();
    const ModelCoefficients coeff = testing::baseline_coefficients();

    const PuckGeometry dry = compress_puck(recipe, coeff, 0.0);
    REQUIRE(dry.compression == Catch::Approx(0.0));
    REQUIRE(dry.depth_m == Catch::Approx(recipe.puck_depth_m));
    REQUIRE(dry.porosity == Catch::Approx(coeff.initial_porosity));

    // Absurd pressure must not compress the puck out of existence.
    const PuckGeometry crushed = compress_puck(recipe, coeff, units::bar_to_pa(100.0));
    REQUIRE(crushed.compression <= coeff.maximum_compression);
    REQUIRE(crushed.depth_m > 0.0);
    REQUIRE(crushed.porosity >= coeff.minimum_porosity);
    REQUIRE(crushed.pore_volume_m3 > 0.0);

    // Monotone: more pressure never makes the puck taller or more porous.
    const PuckGeometry nine_bar = compress_puck(recipe, coeff, units::bar_to_pa(9.0));
    const PuckGeometry two_bar = compress_puck(recipe, coeff, units::bar_to_pa(2.0));
    REQUIRE(nine_bar.depth_m <= two_bar.depth_m);
    REQUIRE(nine_bar.porosity <= two_bar.porosity);
}
